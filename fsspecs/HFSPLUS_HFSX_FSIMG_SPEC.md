# HFS+ / HFSX on-disk filesystem specification and fsimg implementation plan

**Status:** independent, research-based engineering specification (2026-10-19)  
**Target:** `fsimg` — create complete filesystem images and extract their contents, *not* an in-place filesystem editor  
**Formats:** HFS Plus (Mac OS Extended), HFSX (including case-sensitive), HFS+ journaling detection, optional legacy HFS wrapper  
**Source and licensing policy:** This document describes published format facts, numeric constants needed for interoperability, and independently composed algorithms/tests. **No third-party source code, proprietary implementation, or non-BSD-licensed code is copied into it.** Links to GPL/APSL/LGPL/MIT/CPL code are for reading, interoperability and black-box testing only. Anyone porting code into a BSD-only project should check each individual file's license and preserve notices; being linked here is **not** a license clearance.

## 0. Executive summary and implementation contract

HFS+ is a block-allocated, metadata-B-tree filesystem. Its named objects live in a **catalog B-tree**; content lives in one of two **forks** (data and resource); the first eight extents of each fork are inline in catalog records; additional extents live in an **extents overflow B-tree**; another file stores the **allocation bitmap**; extended attributes live in an **attributes B-tree**. The header gives the physical location of all special files. The on-disk format uses big-endian integers, fixed 32-bit file identifiers (CNIDs), and Unicode filenames governed by legacy Apple-specific ordering rules. Journaling is optional. HFSX changes the signature/version and supports binary (case-sensitive) catalog-name ordering. [A1]

For `fsimg` the most useful architecture is:

- **Reader:** image/partition selector -> HFS(+)/HFSX probe -> bounded forks/extents reader -> generic B-tree cursor -> catalog traversal -> name and metadata adapters -> extraction. Extra adapters handle hardlinks, `decmpfs`, and attributes.
- **Writer:** build a complete directory tree and allocate CNIDs -> normalize names and reject compare-equivalent collisions -> allocate user data forks and resource forks -> bulk-pack catalog leaf/index nodes -> build attributes B-tree and extents B-tree as needed -> allocate all metadata files and bitmap -> fill both volume headers last. There is no B-tree mutation or insertion/deletion logic.
- **Validator:** independent format invariants applied to both incoming files and output images. Differentiate *corrupt/unsafe*, *unsupported*, *suspicious but legal*, and *valid*. Optionally expose these as fuzz properties.

### Proposed capability stages

| Stage | Read/extract | Write/create | Validation |
|---|---|---|---|
| 1. Minimal interoperable | HFS+, HFSX; catalog, both forks, contiguous/fragmented extents, POSIX modes, symlinks | Nonjournaled HFS+ and HFSX; both forks, UTF-16 names, hierarchy, empty and large files, correct threads | Header, B-trees, allocation bitmap, catalog references, fork bounds |
| 2. Metadata faithful | Extended attributes; Finder info; raw fork access; hardlinks (regular files) | Inline xattrs; Finder info; file hardlinks | Catalog/attribute consistency; threads; link references |
| 3. Historical and advanced | HFS wrapper; journal detect/policy; compressed files; directory hardlinks | Optional HFS wrapper; large xattrs; possibly hardlinked directories | Damaged/dirty volumes; journal state; deeper cross-references |
| Later, only if demanded | Nonempty-journal replay to an in-memory overlay; uncommon codec support; special policy features | Real journaling; bootable/firmware image extras | Recovery tests and specialized Apple behavior |

**Opinionated default:** write a clean, unjournaled, *unwrapped*, single-volume HFS+ image with 4096-byte allocation blocks, 8192-byte catalog/attribute nodes (4096 is also legal), nonempty catalog tree, an initialized extents tree (even if empty), all newly allocated blocks initialized/zero-padded, and coherent primary/alternate headers. For HFSX select binary vs folded key comparison explicitly; never infer it from the `HX` signature alone.

## 1. Sources, provenance and normative scope

The canonical public *format* reference is Apple's March 2004 Technical Note TN1150 [A1]. Later Apple open-source HFS definitions show features absent or described as reserved in TN1150, most notably inline extended attributes and additional catalog flags [A2]. NetBSD's independent BSD-licensed HFS+ reader [B1] and other cross-platform implementations [C1–C6] are useful cross-checks. This document distinguishes **format rule**, **modern observed convention**, and **fsimg recommendation**. TN1150's statements that attributes are incomplete or certain flag bits reserved reflect its publication date and should not be applied blindly to modern volumes.

This is an interoperability spec, not a claim to cover all undocumented Apple extensions; modern content protection, Fusion/hotfile policy, Apple-specific ACL encoding, and post-2040 expanded timestamp conventions should be preserved or handled cautiously. All numeric fields shown below are big-endian *unless explicitly marked otherwise*. Offsets are byte offsets from the start of the respective record, not absolute image offsets, unless stated.

## 2. Container, recognition, fundamental geometry

### 2.1 Distinguish a filesystem volume from its container

The HFS+ format begins at a **volume base offset**. It may occupy an entire raw image, a partition within a GPT/MBR/APM disk image, or a partition inside a DMG image. `fsimg` should accept `--offset` and preferably use shared partition/container detection; do not treat the start of a DMG file or a partitioned disk as the start of HFS+.

- **Raw volume:** HFS+ volume header at `base + 1024`.
- **Volume inside a partition:** apply the same offset relative to the *partition's* starting byte.
- **GPT Apple HFS partition type GUID:** `48465300-0000-11AA-AA11-00306543ECAC` (the container GUID helps discovery; signature still determines the filesystem). APM types include `Apple_HFS` and `Apple_HFSX`. MBR `0xAF` is a common Apple HFS partition type, but not a reliable proof of HFS+.
- **Legacy HFS wrapper:** an HFS MDB with signature `0x4244` (`BD`) at wrapper base+1024 may embed HFS+ at a different base (section 13).
- **DMG, UDIF, sparsebundle and compressed images:** are *containers*, not HFS+ on-disk structures; decode/attach/convert before presenting the raw filesystem bytes to this driver.

### 2.2 Volume-wide facts

| Parameter | HFS+ | HFSX |
|---|---|---|
| Volume header signature at `base+1024` | `0x482B` (`H+`) | `0x4858` (`HX`) |
| Format version (`u16`) | 4 | 5 |
| Integer byte order | Big-endian | Big-endian |
| Addressed physical allocation blocks | `u32` block indices | Same |
| Allocation block size | Power of two, >=512 bytes; 4096 recommended | Same |
| File logical size | `u64` in fork; avoid signed >2^63−1 for compatibility | Same |
| Filename length | Up to 255 UTF-16 code units, decomposed | Same |
| Case-sensitive names | No, legacy HFS+ casefold | **Optional**: catalog tree `keyCompareType=0xBC` binary vs `0xCF` casefold |
| B-tree node sizes | Power of two 512..32768; catalog/attrs at least 4096 | Same |
| Timestamp basis (classic records) | Unsigned seconds since 1904-01-01 UTC | Same, with newer expanded-times caveats |

Volume length is an input from the image/partition/container, **not** `totalBlocks * blockSize` if the final physical space is not a multiple of the allocation block size. The tail may contain the alternate volume header in otherwise unaddressed bytes. The header occupies 512 bytes. The first 1024 bytes and final 512 bytes are reserved. The alternate header occupies `[volumeBytes−1024, volumeBytes−512)`; reserve any allocation block touching `[0,1536)` and `[volumeBytes−1024,volumeBytes)`. [A1]

**Overflow-safe addressing:** for a block `b`, physical offset `base + u64(b)*blockSize`; require `b < totalBlocks`, `blockCount <= totalBlocks-b`, and that the full requested range lies inside the selected volume. Never add/multiply unchecked. Avoid loading metadata based on forged file sizes.

### 2.3 Dates and legacy/modern exceptions

Classic HFS+ catalog dates and most volume dates are unsigned `u32` seconds since 1904-01-01 00:00 UTC. The Unix conversion offset is **2,082,844,800 seconds**; represent pre-1970 dates with signed time, not an unsigned subtraction. Maximum classic date is **2040-02-06 06:28:15 UTC**. **Exception:** the *volume header* `createDate` is historically interpreted as *local time* for volume identity and should not be subjected to the ordinary UTC conversion automatically. Resolution is seconds; there is no separate nanosecond field in the classic catalog record. [A1]

**Modern extension:** Apple's later HFS definitions assign volume bit 29 to expanded/non-native timestamps and catalog flag `0x1000` to expanded times. These were not specified in TN1150. A reader should expose the raw 32-bit values and a `time_format_unknown/expanded` annotation until verified against the relevant implementation; do not reinterpret them as standard 1904 UTC or invent an expanded format. The default writer should **not** set expanded-times bits. [A2]

## 3. The HFSPlusVolumeHeader: exactly 512 bytes

The primary header starts at byte **1024** from the volume base; the alternate header is a copy at **volumeBytes−1024**. Parse using fixed offsets; never trust native C struct padding. [A1]

| Relative offset | Width | Field | Interpretation |
|---:|---:|---|---|
| 0 | 2 | signature | `H+` or `HX` |
| 2 | 2 | version | 4 or 5 |
| 4 | 4 | attributes | Volume flag bitmap |
| 8 | 4 | lastMountedVersion | Writer identification four-character code |
| 12 | 4 | journalInfoBlock | Allocation block containing JournalInfoBlock, only if journaled |
| 16 | 4 | createDate | Historical local-time semantics |
| 20 | 4 | modifyDate | Classic HFS+ timestamp |
| 24 | 4 | backupDate | Classic timestamp |
| 28 | 4 | checkedDate | Last consistency check |
| 32 | 4 | fileCount | Number of catalog **file records**; excludes special files |
| 36 | 4 | folderCount | Number of catalog **folder records minus root** |
| 40 | 4 | blockSize | Volume allocation bytes per block |
| 44 | 4 | totalBlocks | Count of fully addressable volume allocation blocks |
| 48 | 4 | freeBlocks | Count of zeros in in-range allocation bitmap |
| 52 | 4 | nextAllocation | Hint for future block allocation |
| 56 | 4 | rsrcClumpSize | Default resource-fork clump hint (bytes) |
| 60 | 4 | dataClumpSize | Default data-fork clump hint (bytes) |
| 64 | 4 | nextCatalogID | Candidate next CNID, normally >=16 |
| 68 | 4 | writeCount | Increment when changing the volume |
| 72 | 8 | encodingsBitmap | Legacy Mac encoding-use bits |
| 80 | 32 | finderInfo | Eight 32-bit Finder/boot/volume-identification values |
| 112 | 80 | allocationFile | ForkData (bitmap file) |
| 192 | 80 | extentsFile | ForkData (extent overflow B-tree) |
| 272 | 80 | catalogFile | ForkData (catalog B-tree) |
| 352 | 80 | attributesFile | ForkData (optional attributes B-tree) |
| 432 | 80 | startupFile | ForkData (optional boot file) |

#### Attributes and flags

| Bit | Mask | Meaning and implementation treatment |
|---:|---:|---|
| 7 | `0x00000080` | Hardware-write-lock indicator; historical/runtime significance; reader expose |
| 8 | `0x00000100` | Clean/unmounted (`1` means last unmount flushed correctly); writer set only after all content committed |
| 9 | `0x00000200` | Bad blocks recorded via CNID 5 extents |
| 10 | `0x00000400` | No-cache desired |
| 11 | `0x00000800` | Boot-volume inconsistency (`1` means needs checking); writer clear for clean output |
| 12 | `0x00001000` | CNIDs may have been reused; `nextCatalogID` is only a hint |
| 13 | `0x00002000` | Journaling enabled; JIB pointer meaningful |
| 14 | `0x00004000` | Newer Apple definitions: inconsistent-volume flag (older TN1150 reserved) |
| 15 | `0x00008000` | Software-write-locked volume |
| 29 | `0x20000000` | Newer Apple expanded-times flag; preserve/detect cautiously |
| 30 | `0x40000000` | Newer Apple per-file content protection feature |
| 31 | `0x80000000` | Newer Apple unused-node-fix/zeroing convention |

Unlisted/reserved bits: ignore while reading, initialize zero when creating; preserve if any future in-place editor touches them. For fsimg's new nonjournaled volumes: set bit 8; clear 11, 13, 14; set journalInfoBlock=0. Use a distinct, ideally registered writer four-character code in `lastMountedVersion`; **do not masquerade as Apple's `HFSJ`/`10.0`**. WriteCount should be nonzero and coherent across primary/alternate headers. Mac-created volumes often have Finder volume UUID in `finderInfo[6]` and `[7]`; for a deterministic image choose an explicit stable identifier policy, or random UUID if desired. The Finder boot directory indices 0–5 may be left zero for nonbootable volumes. [A1,A2]

**Sanity checks:** signature/version exact pair; plausible nonzero, power-of-two block size; `totalBlocks` compatible with image size; required special files present; `freeBlocks <= totalBlocks`; all special-file extents inside volume; reserved/header blocks allocated; if a journal bit is set, JIB lies inside the volume; counts checked against catalog when doing full validation. Fallback to alternate header only with an explicit recovery diagnostic; don't silently combine fields from incompatible headers.

## 4. Forks, physical extents and the allocation file

### 4.1 ForkData layout: 80 bytes

Every ordinary catalog **file** has both a data and resource fork. Every special file has one data fork described directly by the volume header. Each fork has:

| Byte | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 8 | logicalSize | Valid bytes in fork |
| 8 | 4 | clumpSize | Optional allocation/clump hint |
| 12 | 4 | totalBlocks | Total allocated blocks across *all* extents, including overflow |
| 16 | 64 | extents[8] | Eight 8-byte descriptors |

Each descriptor is `startBlock:u32, blockCount:u32` (physical allocation-block coordinates). An unused descriptor is `(0,0)`; used descriptors precede zero entries. The physical bytes covered by an extent are `[base + startBlock*blockSize, base + (startBlock+blockCount)*blockSize)`. A zero `startBlock` with a nonzero count is not automatically a hole: volume block zero is a real location (normally reserved); validate it as an illegal overlap for ordinary content. HFS+ has **no intrinsic sparse/hole extent marker**. [A1]

- Require `logicalSize <= totalBlocks*blockSize` and physical extent ranges in-bounds.
- The sum of all descriptors, including extents overflow records, must equal `totalBlocks`; allocated capacity may exceed the logical size due to clumps.
- The first eight extents stay in the fork; only subsequent extents use the overflow B-tree.
- File data is read in **logical fork order** across the stored extent sequence, never by sorting physical `startBlock` values.
- `totalBlocks=0` should imply `logicalSize=0` and all first eight descriptors zero for ordinary forks.
- Two different allocated forks or metadata files may not own the same block (except aliasing that is explicitly *metadata-level* hardlink indirection, not duplicate extents).

### 4.2 Special file IDs and forks

| CNID | Use | Header fork |
|---:|---|---|
| 1 | Parent of root (reserved synthetic identity) | None |
| 2 | Root folder | Catalog record |
| 3 | Extents overflow | Header `extentsFile` |
| 4 | Catalog | Header `catalogFile` |
| 5 | Bad blocks (pseudo-file) | Overflow extent keys, no catalog entry |
| 6 | Allocation bitmap | Header `allocationFile` |
| 7 | Startup | Header `startupFile` |
| 8 | Attributes | Header `attributesFile` |
| 13 | Runtime-only special association in later Apple code, not an on-disk catalog file | None |
| 14–15 | Repair/exchange reserved IDs | Do not assign to user files |
| >=16 | Normal user file/folder IDs | Catalog records |

An absent optional attributes/startup file is commonly represented by a zero-length/zero-extent fork. The extents overflow file may be a valid empty B-tree. Avoid giving the extents B-tree itself more than eight extents, otherwise resolving its own overflow extents is circular. [A1,A2]

### 4.3 The allocation bitmap

`allocationFile` contains one bit for every allocation block in the entire volume, including metadata. Block `b` corresponds to byte `floor(b/8)`, bit `7-(b mod 8)`; `1` means used, `0` free. The bitmap file must contain at least `ceil(totalBlocks/8)` logical bytes; its physical fork may consume a whole number of allocation blocks. **All bits beyond totalBlocks are zero, not one**. All physical blocks of the bitmap itself, B-tree files, forks, headers/reserved areas, journal if present, and bad-block pseudo-file must be marked used. `freeBlocks` counts only usable in-range zero bits. [A1]

The last physical bytes not included in `totalBlocks*blockSize` remain outside allocation-space accounting even if an alternate header lives there. Fresh writer: keep a temporary allocation set/interval map, allocate files, then serialize the final bitmap and compute exact `freeBlocks` from that set (don't try to calculate freeBlocks by subtraction from an incomplete initial estimate).

### 4.4 Extents overflow tree

Overflow key: **12 bytes including the key-length prefix**, all numeric values big-endian:

| Offset | Size | Field | Description |
|---:|---:|---|---|
| 0 | 2 | keyLength | 10 (does not include this field) |
| 2 | 1 | forkType | 0 data; `0xFF` resource |
| 3 | 1 | pad | Zero for new images |
| 4 | 4 | fileID | Owning file's CNID or special file CNID |
| 8 | 4 | startBlock | **Logical offset into fork, in allocation blocks** |

Leaf payload: eight extent descriptors, **64 bytes**. Comparison is tuple `(fileID unsigned, forkType unsigned, startBlock unsigned)`, *not* physical ordering, and **not** the byte order of fields in the key. If first eight descriptors cover `k` blocks, the first overflow record should have startBlock=`k`; the next must equal that plus the number of blocks in the preceding overflow record. Reject missing record spans, overlapping logical offsets, or overflow data following an already complete fork. Pseudofile CNID 5 always uses forkType 0. [A1]

Reader lookup for fork byte offset `p`:

1. Calculate `q=floor(p/blockSize)`; check `p < logicalSize`.
2. Walk the fork's initial extents, subtracting block counts from `q` until a match.
3. Otherwise search the extents B-tree for the greatest `(fileID,forkType,startBlock) <= (fileID,forkType,q)`; verify both fileID and forkType match, then use its eight descriptors.
4. Compute the exact block offset inside that extent. Cap reads at the logical end, extent end, physical image end, and caller's read size.
5. Detect a cycle or repeated overflow-key startBlock, zero-length used descriptor, and excessive iteration (bound by totalBlocks and available B-tree records).

For a new, contiguous image, a single extent per fork suffices. To test overflow support, deliberately fragment a fork into at least nine physical spans, independent of file logical size.

## 5. Generic HFS+ B-tree format

Three primary trees exist: catalog, extents, attributes. They share exact node framing but have different key comparators and record layouts. Each tree is a logical byte stream consisting of power-of-two fixed-size nodes (512 to 32768 bytes). Node number `n` is at byte `n * nodeSize` *within its B-tree fork*, not at physical block `n`. Tree node size need not equal allocation block size. Catalog and attribute nodes must be at least 4096 bytes. [A1]

### 5.1 Node descriptor and record directory

Every node has a **14-byte descriptor**:

| Byte | Width | Name | Semantics |
|---:|---:|---|---|
| 0 | 4 | fLink | Next node at same level, or first map node from header |
| 4 | 4 | bLink | Previous node at same level; zero from header |
| 8 | 1 signed | kind | `−1` leaf, `0` index, `1` header, `2` map |
| 9 | 1 | height | Leaf 1, index >1, header/map 0 |
| 10 | 2 | numRecords | Number of records stored |
| 12 | 2 | reserved | Initialize zero |

Data records grow forward from offset 14. An offset array grows **backwards** from the node's end. For `N` records there are `N+1` offset entries:

- `offset[i] = BE16(node[nodeSize−2*(i+1) ...])`, `0 <= i <= N`.
- `offset[0]` is 14; `offset[i]` is record `i`'s start; `offset[i+1]` is record `i`'s end.
- `offset[N]` is the first free byte after the last record.
- Require `14=offset[0] <= offset[1] <= ... <= offset[N] <= nodeSize−2*(N+1)`, and sufficient size for the record's key/payload.
- Offsets and padded data should be even; reserve a pad byte if the preceding key or payload has odd length. At least two bytes of footer remain per entry.

This final free-space offset is easy to omit incorrectly. Header/map nodes use the same framing. Some implementations allow empty leaf records or empty key cases; carefully distinguish valid empty B-trees from a malformed node with no room to hold a key.

### 5.2 B-tree header: node 0

Node 0 must be kind `1`, height `0`, `numRecords=3`. Its three records are:

1. A 106-byte `BTHeaderRec` starting at offset 14.
2. A 128-byte user-data record starting at offset 120 (unused/zero for the three standard trees).
3. A node allocation bitmap/map record starting at offset 248 and ending at `nodeSize−8`; size `nodeSize−256`.

The four record-offset entries for node0 reside in its final eight bytes. Its descriptor `fLink` points to the first *map node* if the map record cannot cover all tree nodes; `bLink=0`. The map bitmap uses the same most-significant-bit-first convention as the volume allocation bitmap. [A1]

`BTHeaderRec` fields at offsets **relative to the beginning of record 1**:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 2 | treeDepth | Height of root; zero for empty tree |
| 2 | 4 | rootNode | Root node number; may be leaf |
| 6 | 4 | leafRecords | Total leaf record count |
| 10 | 4 | firstLeafNode | First leaf (0 when none) |
| 14 | 4 | lastLeafNode | Last leaf (0 when none) |
| 18 | 2 | nodeSize | Fixed tree node size |
| 20 | 2 | maxKeyLength | Key bytes excluding initial keyLength |
| 22 | 4 | totalNodes | Entire logical tree including free nodes |
| 26 | 4 | freeNodes | Unused tree node count |
| 30 | 2 | reserved1 | Zero for creation |
| 32 | 4 | clumpSize | Legacy hint; fork clumpSize is authoritative |
| 36 | 1 | btreeType | 0 for catalog/extent/attrs; 128 for hot-file tree |
| 37 | 1 | keyCompareType | Catalog HFSX: `0xBC` binary, `0xCF` folded; otherwise normally unused |
| 38 | 4 | attributes | B-tree flag bitmap |
| 42 | 64 | reserved3 | 16 × u32; zero when creating |

B-tree header flags: `0x02` means 16-bit key lengths (must be set for HFS+); `0x04` means index keys are stored at variable actual length (must be set for catalog, must be clear for extents). For attributes follow accepted modern test images and current formatter convention; build variable keys with flag 0x04 unless compatibility tests show another requirement. `0x01` historically signals bad close, not a format-version discriminator. Empty extents/attributes tree: `rootNode=firstLeafNode=lastLeafNode=treeDepth=leafRecords=0`; node 0 still exists. [A1,A2]

A map node has descriptor kind 2/height 0, **one** map record beginning at 14, typically `nodeSize−20` bytes long (the final two bytes of its payload area may remain free to maintain alignment). Map node `fLink` leads to next map node; mark map nodes themselves allocated in the map bitmap. **Do not assume a single header map fits every tree**; an 8192-byte header map accommodates `(8192−256)*8=63488` node bits, beyond which chained map nodes are required.

### 5.3 Leaf and index records; searching

Every keyed record starts with `u16 keyLength` (excluding its own two bytes), followed by its key bytes; pad if needed to put data on a 2-byte boundary. **Leaf** payload = filesystem-specific data; **index** payload = single 4-byte child node number. For a fixed-index-key tree, index keys physically reserve `maxKeyLength` bytes even if keyLength is shorter; in a variable-index tree, reserve just the specified bytes and padding. Data should occupy even bytes, padded at record end if needed.

HFS+ indexes are *not* conventional `N keys, N+1 children`: each record associates the **smallest key of its child subtree** with that child node. To search, choose the **greatest index key <= search key** and follow its child. If the search key precedes the first entry, it is absent from that subtree. Recurse until a leaf and test equality under the correct comparator. Every level of leaf/index nodes has valid bidirectional sibling links. [A1]

### 5.4 Bulk construction algorithm for fsimg

1. Assemble every leaf `(key, data)` record, encode key and data, and sort with the tree's exact comparator.
2. Detect duplicate compare-equal keys; fail or remap deterministically. Include catalog threads in the global sort.
3. Pack consecutive records into leaf nodes, including 14-byte descriptor, offsets, and `N+1` footer offsets; never split one record. Set heights=1 and correct `fLink`/`bLink` siblings.
4. Form one parent index record per child node: `(child's *minimum* key, childNodeNumber)`. Pack index nodes in ascending order. Repeat until a single node remains; it becomes root.
5. Create node0 header and any map nodes, assigning stable node numbers and marking used nodes. Account for any reserved/free growth nodes in totalNodes/freeNodes and node bitmap.
6. Set `treeDepth`, `leafRecords`, `rootNode`, `firstLeafNode`, `lastLeafNode` and `totalNodes`; serialize the whole tree as a single contiguous logical fork where feasible.
7. Cross-check `treeBytes = totalNodes*nodeSize`, node numbers, and all index first-key invariants before writing.

Choose a consistent layout of node numbers (e.g. 0 header, then leaf nodes, then parent levels, then map nodes), but do not hard-code any assumption about their physical ordering in the reader. When map nodes need to be added, include their count before finalizing map capacity; an easy way is a small fixed-point loop on totalNodes and required map nodes. Set unused bytes to zero for deterministic, clean image generation.

## 6. Catalog: names, folders, files, threads

The catalog is the namespace and object metadata database. Every normal object has one **primary** record keyed by `(parentCNID, name)` and one **thread** record keyed by `(its own CNID, empty name)`. The root folder has CNID **2**, parent CNID **1**, and a name representing the volume label; its own thread key is `(2,"")`. A file/folder's children are all adjacent in catalog-key sort order because their keys have the same parentCNID. [A1]

### 6.1 Catalog key: 8 + 2n bytes on disk

| Offset | Width | Field | Constraint |
|---:|---:|---|---|
| 0 | 2 | keyLength | `6 + 2*n`, maximum 516 |
| 2 | 4 | parentID | Parent CNID for files/folders; own CNID for threads |
| 6 | 2 | nameLength | `n` UTF-16 code units (0..255) |
| 8 | `2*n` | nameUnits | Big-endian UTF-16; HFS decomposed normal form |

Catalog comparator: first compare `parentID` numerically as unsigned `u32`; then compare name by HFS+ **legacy folded** ordering or HFSX binary/folded as indicated in section 7. Length and raw key bytes are *not* general-purpose correct sort keys. Primary record names must be nonempty; thread records use empty-name key. [A1]

### 6.2 Leaf payload record types

| Type (`u16` at payload+0) | Meaning | Payload length |
|---:|---|---:|
| `0x0001` | Folder primary | 88 bytes |
| `0x0002` | File primary | **248 bytes** |
| `0x0003` | Folder thread | `10 + 2*n` bytes |
| `0x0004` | File thread | `10 + 2*n` bytes |

**Folder payload, relative offsets:**

| Offset | Width | Name |
|---:|---:|---|
| 0 | 2 | recordType =1 |
| 2 | 2 | flags |
| 4 | 4 | valence (immediate direct children: files + folders; not threads) |
| 8 | 4 | folderID (CNID) |
| 12,16,20,24,28 | 5×4 | create, contentMod, attributeMod, access, backup dates |
| 32 | 16 | HFSPlusBSDInfo permissions |
| 48 | 16 | FolderInfo (Finder data) |
| 64 | 16 | ExtendedFolderInfo |
| 80 | 4 | textEncoding hint |
| 84 | 4 | **folderCount** when `flags & 0x0010` (especially modern HFSX); otherwise historically reserved |

**File payload, relative offsets:**

| Offset | Width | Name |
|---:|---:|---|
| 0 | 2 | recordType =2 |
| 2 | 2 | flags |
| 4 | 4 | reserved1; may be repurposed in modern hardlink chains |
| 8 | 4 | fileID (CNID) |
| 12,16,20,24,28 | 5×4 | create, contentMod, attributeMod, access, backup dates |
| 32 | 16 | HFSPlusBSDInfo permissions |
| 48 | 16 | FileInfo (Finder info) |
| 64 | 16 | ExtendedFileInfo |
| 80 | 4 | textEncoding |
| 84 | 4 | reserved2 |
| 88 | 80 | dataFork |
| 168 | 80 | resourceFork |

**Thread payload**, variable length: `recordType:u16 (3 or 4), reserved:u16, parentID:u32, nameLength:u16, nameUTF16BE[2*n]`. A thread's key has empty name and its own CNID; the **thread payload** stores the primary record's actual parent and actual filename. Always generate a thread for both files and directories; it is essential for reverse lookup, robust link traversal, and macOS interoperability. [A1]

### 6.3 Catalog flags

The initial TN1150 listed only file flag `0x0001` (locked) and `0x0002` (thread present). Later Apple definitions have more meanings for file/folder flags [A2]:

| Mask | Meaning / writer guidance |
|---:|---|
| `0x0001` | File locked; don't set by default |
| `0x0002` | File thread exists; **set on all created files** |
| `0x0004` | Has extended attributes; set if xattrs really exist |
| `0x0008` | Has security/ACL attribute; set only when appropriate |
| `0x0010` | Folder has valid `folderCount` (HFSX convention) |
| `0x0020` | Hardlink-chain marker (advanced) |
| `0x0040` | Contains child directory hardlink (advanced) |
| `0x0080` | Finder info date-added field meaningful |
| `0x1000` | Expanded timestamp representation (modern Apple extension) |

Higher/other modern bits include Fusion-drive storage hints; preserve when reading, initialize only known bits when writing. `valence` is always a count of **primary direct child entries** and must match the catalog. `folderCount` is a *different* count (direct subfolders) and only valid when its flag is set; don't confuse it with `valence`. The root counts as a folder record but is excluded from volumeHeader.folderCount.

### 6.4 BSD info: 16 bytes at folder+32 / file+32

| Offset | Width | Field | Use |
|---:|---:|---|---|
| 0 | 4 | ownerID | POSIX UID |
| 4 | 4 | groupID | POSIX GID |
| 8 | 1 | adminFlags | Root-controlled BSD flags |
| 9 | 1 | ownerFlags | User BSD flags; includes compressed bit `0x20` |
| 10 | 2 | fileMode | POSIX type and permission bits |
| 12 | 4 | special | Device ID, link count, or hardlink reference depending on object kind |

Conventional POSIX type masks: directory `040000` (octal), regular `0100000`, symlink `0120000`, FIFO `0010000`, character device `0020000`, block device `0060000`, socket `0140000`. `S_IFMT` is octal `0170000`. Do not treat `fileMode & S_IFMT == 0` as corruption: older HFS+ creators left BSD info uninitialized; Mac systems apply defaults then. `ownerFlags` bit `0x20` is typically `UF_COMPRESSED` (file may have `com.apple.decmpfs`). Extraction should expose permissions and flags; apply unsafe ownership/device-node recreation only with explicit opt-in. [A1,A2]

### 6.5 Finder info: preserve, do not discard

For files the 16-byte `FileInfo` includes 4-byte file-type FourCC, 4-byte creator FourCC, 2-byte Finder flags, 4-byte icon position (two signed 16s), and 2-byte reserved; the next 16-byte extended finder info has legacy Finder metadata. FolderInfo uses an 8-byte window rectangle, 2-byte flags, 4-byte icon position and 2-byte reserved. The paired 16-byte extended blocks contain additional positioning/flags data. On macOS a synthetic `com.apple.FinderInfo` xattr may expose these **32 catalog bytes**, rather than a separate entry in the attribute B-tree. Preserve raw Finder bytes even if fsimg does not interpret every subfield. Custom icon, invisibility, bundle/package and alias indicators matter to Finder. The data fork should not be mistaken for a Finder alias by name alone. [A1]

For image creation use zeroed Finder info by default; accept explicit raw Finder info in the fsimg metadata interface. If a user supplies `com.apple.FinderInfo`, map it to the catalog's 32-byte region rather than creating a redundant arbitrary-xattr record, unless proven necessary by an interoperability fixture.

### 6.6 Catalog traversal and integrity

Reader operations:

- **Lookup:** start from root CNID2, compose `(parent,normalizedName)` keys, tree lookup using matching comparator.
- **List dir:** range scan from `(dirCNID,empty)` to parentID transition; skip the directory thread key `(dirCNID,empty)` and any other thread records; return file and folder primary records only.
- **Resolve CNID:** find thread `(CNID,empty)`, read its parent+name, look up corresponding primary. Detect thread cycles and broken backlinks.
- **Resolve root:** primary `(1,volumeName)` and root thread `(2,empty)`; don't infer the label from the main header since it has no dedicated string label field.
- **Check:** unique primary CNIDs, unique catalog keys under comparator, exactly one thread per primary, threads point back to same primary, folders' `valence` matches child count, objects reference valid parent folder CNIDs, no directory cycles, root CNID=2, `fileCount/folderCount` match counted primary records.

For corruption-handling provide an option to enumerate physical catalog leaves and report orphan/corrupt entries, but do not follow arbitrary parent pointers or symlink targets during extraction by default.

## 7. Unicode, casefold and filename edge cases

This is a **critical compatibility requirement**, especially for a writer. HFS+ is *not* UTF-8 on disk, *not* ordinary modern NFC/NFD, and *not* generic Unicode casefold. [A1,B1]

1. Keys contain Unicode as big-endian **UTF-16 code units**; names use a `u16` count of code units, maximum 255, including any surrogate pairs as two units.
2. HFS+ stores **canonical decomposition and canonical combining order**, with compatibility behavior based on historical Unicode 2.1 (older volumes) and Unicode 3.2 (Mac OS X 10.3 onward). Do not apply Unicode compatibility normalization (NFKD).
3. Exceptions: characters in **U+2000..U+2FFF** and **U+F900..U+FAFF** are not decomposed by the HFS+ rules. Precomposed Hangul U+AC00..U+D7A3 is decomposed into conjoining Jamos.
4. The HFS+ case-insensitive comparator uses Apple's legacy case-fold/ignorable table (`FastUnicodeCompare` semantics). Ignorable characters can compare as absent. A modern `str.casefold()` or `UnicodeData` from the system is *not* necessarily equivalent.
5. On HFSX, inspect **catalog** B-tree `keyCompareType`: `0xBC` means compare unsigned 16-bit code units lexicographically; `0xCF` means legacy HFS+ folding. HFSX is not necessarily case-sensitive. Binary comparison does **not** discard ignorable code points. Equal prefix sorts the shorter string first.
6. Casefold-equivalent distinct input names cannot coexist in one HFS+ folder; reject or explicitly disambiguate instead of silently dropping one.
7. A stored NUL code unit normally should not appear in a user-visible filename, but is historically used by special private metadata directory names. Keep the raw name and don't truncate it as a C string. Keep parser and output-path sanitization separate.
8. The HFS+ name namespace historically maps **colon (`:`) / slash (`/`) differently from POSIX**: the on-disk/Mac convention reserves colon as a path separator while POSIX reserves slash. macOS performs a colon↔slash translation for presentation. Thus a POSIX filename containing a literal colon commonly appears on disk with a slash code unit, and vice versa. Treat this as an explicit configurable boundary translation; never use raw on-disk units directly as a host OS output path.
9. Surrogate errors: a strict writer should reject malformed UTF-8 and unpaired surrogate units. A recovery reader can expose invalid code-unit names as escaped raw values, but must not collapse different broken names into the same host path.

**Suggested implementation:** centralize `hfs_decompose(input,unicodeVersion)` and `hfs_compare_folded(utf16A,utf16B)` plus `hfs_compare_binary` in one testable module. Freeze a specific Unicode 3.2 dataset for creation and document behavior for older 2.1 names. You can consult NetBSD's BSD-2-Clause HFS Unicode-related implementation and independently construct tests, but don't import casefold tables from non-BSD code without a license review. Store original raw names separately from the host display path. Add a property: `normalized(writer(name))` must be identical to the serialized key, and catalog sort must agree with lookup comparison for every valid pair.

### Unicode test cases

- Precomposed `é` vs `e`+U+0301; Greek decompositions; Hangul U+AC00 and equivalent conjoining Jamos.
- Case variants `A/a`, `Straße/STRASSE` (*do not assert generic Unicode folding equals HFS semantics*); mixtures of ignorable code points.
- CJK compatibility ideographs U+F900..FAFF, symbols U+2000..2FFF that normal modern Unicode decomposers may change.
- Prefix names (`a`, `aa`), maximum 255 UTF-16 units, supplementary-plane surrogate pairs, names whose UTF-8 encoding exceeds 255 bytes.
- Colon/slash round trip; pathological empty/all-ignorable normalized keys; `.`/`..`; reserved synthetic directory with initial NULs.
- Both HFSX comparator values; on binary volumes create `A` and `a` side-by-side; on folded volumes reject the equivalent collision.

## 8. Extended attributes B-tree (modern HFS+)

**Important version distinction:** TN1150 (2004) calls inline attribute records reserved and says the complete attribute-key definition was still in flux. Modern Apple HFS format definitions and real macOS volumes use a **finalized attribute key and inline-record form**. For modern read/write interoperability prefer this section over that historical part of TN1150. These are serialized record facts, not copied source code. [A1,A2]

### 8.1 Attribute key: fixed prefix, variable UTF-16 name

Attributes are associated with an object **CNID**, not with a pathname, so renaming an object does not rewrite its xattr keys. Attribute keys use a different name limit from catalog keys.

| Byte offset | Width | Meaning |
|---:|---:|---|
| 0 | 2 | `keyLength` = `12 + 2*n` (excludes this length field) |
| 2 | 2 | `pad`, normally zero |
| 4 | 4 | `fileID` / CNID of object |
| 8 | 4 | `startBlock`: 0 for the primary inline/fork record, logical allocation-block offset for attribute-overflow extents |
| 12 | 2 | `attrNameLen` = `n`, up to **127** UTF-16 code units |
| 14 | 2*n | Attribute name, UTF-16BE |

Minimum key size **14 bytes including length**, maximum **268 bytes including length**. This is deliberately distinct from catalog's 8+2*n. Modern Apple header gives `kHFSMaxAttrNameLen=127`. Some old descriptions use an obsolete structure or claim name-length limits different from modern implementations; use test fixtures to settle any compatibility policies. [A2]

**Key comparator:** compare `fileID`, then the Unicode attribute name in its attribute-tree comparison order, then `startBlock`. For a portable reader, implement an explicitly selected attributes-name comparator; the contemporary HFS+ implementation generally compares attribute-name code units *binary* (not catalog-name case folding). For a writer, reject duplicate names under the comparator and independently verify it against an Apple-created multi-attribute fixture (names differing only by case are a good test). Don't reuse the catalog comparator without proof. Note that field *serialization order* differs from comparator order because `startBlock` precedes `attrName` in the key's bytes. [A2,C1]

Most ordinary values have `startBlock=0`. If an attribute's allocated fork spans more than eight extents, its continuation records share the `fileID` and `attrName` but use increasing **logical** `startBlock`. Do not confuse the attribute-tree key's `startBlock` with a physical disk block number.

### 8.2 Leaf payload variants

The first 4 bytes of any attribute-tree leaf payload are `recordType` (big-endian):

| Record type | Layout from payload start | Total fixed-size header |
|---|---|---:|
| `0x00000010` inline xattr | `type:u32; reserved0:u32; reserved1:u32; dataSize:u32; data[dataSize]` | 16 bytes before data |
| `0x00000020` fork xattr | `type:u32; reserved:u32; HFSPlusForkData` | 88 bytes total |
| `0x00000030` overflow extents | `type:u32; reserved:u32; extentDescriptors[8]` | 72 bytes total |

All values above are in big-endian; raw xattr payload bytes have their own per-attribute format and may use other endianness. For the inline record, `dataSize` is the number of actual payload bytes; record padding **is not** part of the user value. The former short `HFSPlusAttrInlineData` declaration encountered in older sources is obsolete; parsing it in place of the modern 16-byte-prefix record loses/corrupts data. Unknown leaf types: preserve/report them, do not mistake them for zero-length attributes. [A2]

- **Inline:** small xattrs reside entirely in one leaf. Enforce `16+dataSize` fits the record extent and a configured maximum. B-tree node size constrains the largest inline value; the maximum depends on the key's UTF-16 length, node descriptor, offset directory and possible padding. Do *not* assume all 4 KiB attributes are inline in 4 KiB B-trees.
- **Fork-backed:** the 80-byte ForkData locates bytes on disk; resolve its first eight extents then add `0x30` continuation records for any extra extents. Verify logicalSize and totalBlocks using the same rules as user forks; these blocks belong to the attribute fork, not to the owning file's data fork.
- **Extents continuation:** validate the same startBlock continuity, block accounting, and uniqueness as the extents-overflow tree. Watch for multiple keys with equal `(fileID, attrName)` but different startBlock.
- **Missing attributes tree:** zero extent for the attributes special file means no on-disk attributes tree; this must not block ordinary catalog extraction. On read-only recovery allow catalog xattr flags to disagree with the tree but report inconsistency.

### 8.3 Important well-known attributes and special handling

| Name | Interpretation / policy |
|---|---|
| `com.apple.decmpfs` | Transparent file compression; requires separate `decmpfs` decoder (section 11) |
| `com.apple.ResourceFork` | Usually a virtual xattr view of the **catalog resource fork**, not a second copy that must be stored in the attributes B-tree |
| `com.apple.FinderInfo` | Usually virtual view of catalog FileInfo+ExtendedFileInfo or FolderInfo+ExtendedFolderInfo (32 bytes) |
| `com.apple.system.Security` | Apple filesystem ACL/security blob; opaque preservation only in v1; require OS-specific parser to interpret |
| `com.apple.quarantine` | User/application-defined metadata; treat as opaque bytes |
| `com.apple.metadata:*` | Application metadata; opaque bytes; round-trip if possible |
| `com.apple.provenance` | Modern macOS metadata; may appear on volumes; preserve as opaque |

A portable interchange layer should distinguish **physical disk-stored xattrs** from **synthetic macOS `listxattr` views**. Otherwise extracting then recreating from `getxattr` may duplicate Finder info or resource forks, and might change security metadata. Expose logical xattr names to callers along with a `backing_kind` (`attributes_inline`, `attributes_fork`, `catalog_finderinfo`, `resource_fork_synthetic`, `unknown`). [A2,C1]

### 8.4 Writer recipes and integrity

For a small ordinary xattr `attr`, create a single `0x10` leaf record with key `(CNID, attrName, 0)`, set catalog `hasAttributes` flag, and insert the key in global B-tree order. For a large xattr, allocate disk blocks, construct a `0x20` fork record and, if needed, `0x30` overflow records, update the allocation bitmap and node maps. `dataSize` and fork `logicalSize` are byte counts, independent of name unit count. All xattr content is **binary**, not necessarily UTF-8 or NUL-terminated. Security xattr flags are distinct from generic xattr presence. The simplest first writer can reject individual xattr data that exceeds available inline payload and offer a later `--large-xattrs` capability, rather than emit broken fork records.

Check that every attribute CNID resolves to a catalog primary record or is a documented special object; verify attribute fork block ownership and catalog xattr flags; detect duplicate keys and missing fork continuation records. In forensic extraction, allow dangling attributes to be reported and optionally exported in a salvage manifest.

## 9. POSIX object types, regular hard links and directory hard links

### 9.1 Object type and content mapping

- **Regular file:** catalog file record, mode regular, data fork contains file bytes; optional resource fork and xattrs.
- **Directory:** catalog folder record, no data/resource fork in folder payload; child records in catalog, plus catalog thread.
- **Symbolic link:** catalog *file* record with POSIX `S_IFLNK`, target bytes in data fork; see section 10.
- **FIFO/socket/device:** catalog file record with appropriate `S_IF*` mode. Device number may be stored in `permissions.special`. Exposing special nodes for extraction is different from automatically creating devices on host OS; default is emit metadata only.
- **Regular hardlink:** the directory entry is a *stub catalog file record* referring to an indirect-node file in a private directory; multiple stubs refer to the same logical content object. Each stub has its own CNID and a thread; two hardlink paths are **not** generally two ordinary catalog file records sharing the same fileID.

### 9.2 Regular hardlinks (older/common HFS+ representation)

A private metadata folder exists beneath root. Its on-disk name starts with **four U+0000 code units**, followed by `HFS+ Private Data`. It is intentionally hidden from ordinary namespace enumeration. Its CNID is discoverable via a catalog lookup, *not* a fixed universal number. The physical file providing a hardlinked file's data may be stored under `iNode<decimal-linkReference>` in that directory. Such a file is called the **indirect-node file**. [A1,A2]

A visible hardlink stub has the Finder four-character file type **`hlnk`** and creator **`hfs+`**. Its `HFSPlusBSDInfo.special` holds a nonzero **link reference number**, not the target's CNID. Resolve by forming the `iNodeNNN` name in the private metadata folder; the resulting catalog primary provides the data/resource forks, ownership, and possibly true metadata. Link count is recorded on the indirect object in the appropriate special/metadata field. Modern macOS implementations add optional hardlink chaining flags and bookkeeping; do not require those newer fields on old images and do not invent them when writing. [A1,A2]

Reader resolution:

1. Identify `hlnk`/`hfs+` marker **along with** appropriate catalog/volume conventions; don't treat every four-character type coincidence as trusted content metadata.
2. Find private metadata folder, then primary record for `iNode` plus decimal nonzero linkRef.
3. Verify target is appropriate type and within catalog; avoid arbitrary recursive indirection. Return the visible path but share its resolved file data object with other entries of same reference.
4. Track encountered references, compare declared link count against number of visible stubs, and report dangling refs as corruption/salvage.
5. Preserve the stub CNID and indirect file CNID in raw metadata; represent logical inode identity independently from either of them.

**Writer:** Stage 1 should reject multiple hardlink paths as unsupported or expand them into independent regular files **only with an explicit lossy option**. In stage 2, construct one indirect file, generate an `iNode` name, create one stub catalog file per hardlink, assign independent CNIDs and threads, set relevant link-count and flags fields, and verify the result with a native macOS `stat`/`ln` round-trip. The simple `iNodeNNN` representation is not a generic implementation of hardlinks on all versions of macOS; use native-generated regression fixtures.

### 9.3 Directory hardlinks

Directory hardlinks are a specialized macOS feature, encountered particularly in older Time Machine backups. They are more complex than file hardlinks: private directory records, surrogate directories, ancestry and link-count flags exist to preserve apparent directory trees. Apple's later public source references a special private directory name **`.HFS+ Private Directory Data\r`** (literal CR at end) and indirect names with prefix **`dir_`**. Some secondary explanations omit punctuation or the CR, so **test against real images**. [A2,C2]

The reader must not blindly follow surrogate directories, otherwise it can produce cycles or multiply a large subtree. It should expose the raw surrogate and optionally resolve under an explicit policy with a **visited-identity set**, maximum depth, hardlink count, and path explosion limit. A standard POSIX tar archive cannot faithfully describe every directory-hardlink topology. First writer: unsupported; document omission clearly. For a reader that does not yet resolve them, returning a diagnostic placeholder is safer than silently losing contents.

### 9.4 Other hidden operational objects

Older versions store some open-but-unlinked file content as entries in the private directory named `temp<decimal-CNID>`. These are not ordinary user files and can be artifacts of dirty/unmounted volumes. Native macOS may also reserve special files such as `.HFS+ Private Directory Data`, `.journal`, `.journal_info_block`, `.hotfiles.btree`, and `.Spotlight-V100`; do not assume every one is a filesystem primitive rather than a normal catalog object or generated system file. Whether to hide, include, or report private entries must be configurable. Do not delete files during read/extraction.

## 10. Symbolic links and Mac name/path semantics

In an HFS+ symlink, `permissions.fileMode` includes **`S_IFLNK`**, Finder file type is conventionally `slnk`, creator `rhap`, and **data fork contains the link's pathname bytes**. Resource fork is normally empty. Modern Unix clients treat the target as UTF-8; older systems have subtleties. A correct parser uses the data fork's **logical byte length** (not NUL termination) to obtain the target. Never interpret the link bytes as a catalog key without explicit path normalization. [A1]

On extraction:

- Store target as string/bytes metadata and, if requested, create a symlink **without following it**. A target beginning `/` or containing `..` can escape the extraction directory if subsequently traversed; safe extractors should not traverse through newly created symlinks when creating later children.
- Reject embedded NUL for the host `symlink()` API, or export target as escaped bytes in a manifest; keep the original for lossless round trips.
- macOS POSIX `/` path-component separation and HFS on-disk colon/slash translation are distinct from symlink target bytes. Do not universally swap slashes and colons *inside symlink content*.
- Resource forks can be exposed through a named fork path on Apple systems (`.../..namedfork/rsrc`) or as `com.apple.ResourceFork` through certain interfaces. For fsimg, present resource bytes as a first-class data stream.

A host's casefold rules, Unicode normalization, forbidden characters, and length limits may be narrower than an HFSX image. Extraction must not clobber colliding filenames (`File`/`file` on a case-insensitive host, `é`/decomposed sequence on a normalizing host). Provide an explicit escape/mangle mapping and machine-readable collision map, or stop with a collision error. Securely anchor extraction beneath a directory FD and use `openat`/`mkdirat` with no-follow protections where supported.

## 11. `decmpfs` transparent compression

**This is essential for faithfully reading many macOS-created images.** Some regular files flagged compressed (`ownerFlags & 0x20`, commonly `UF_COMPRESSED`) contain no ordinary data-fork bytes. The user-visible uncompressed content instead comes from the extended attribute `com.apple.decmpfs` and sometimes an encoded **resource fork**. A raw-fork-only extractor will falsely produce an empty file; treat this as a functional incompatibility, not an optional cosmetic issue. [C1,C2,D1]

### 11.1 Compression metadata header (16 bytes)

The first 16 bytes of `com.apple.decmpfs` are a special mixed-endian structure:

| Byte | Length | Interpretation |
|---:|---:|---|
| 0 | 4 | Signature bytes `66 70 6d 63` (ASCII `fpmc`, i.e. the little-endian encoding of numeric `cmpf`) |
| 4 | 4 | `compressionType` **little-endian** |
| 8 | 8 | `uncompressedSize` **little-endian** |
| 16 onward | variable | Optional inline compressed/uncompressed content |

Do not swap the signature based on the HFS+ volume's big-endian convention. Typical types:

| `compressionType` | Codec | Storage location |
|---:|---|---|
| 1 | Stored/raw | Inline in the decmpfs xattr |
| 3 | zlib (deflate) | Inline in the decmpfs xattr |
| 4 | zlib | Compressed chunks in resource fork |
| 7 | LZVN | Inline in the decmpfs xattr |
| 8 | LZVN | Chunked resource fork |
| 11 | LZFSE | Inline in the decmpfs xattr |
| 12 | LZFSE | Chunked resource fork |

Other values are known in some implementations and OS revisions, including dataless/hydration-related types; return `unsupported_codec/type` rather than assuming zlib. Not every “raw” encoding means an intact ordinary data fork. [D1,C2]

### 11.2 Inline compression

For inline values, the encoded bytes follow the 16-byte header, unless the type documents a different subformat. Decoder output **must** be exactly `uncompressedSize`, within configurable limits. For type 3, some Apple's implementations encode small non-deflatable content using a leading marker byte rather than a zlib stream; support that exception only after fixture tests, not by accepting arbitrary invalid compressed payloads as literal content. Avoid streaming decompression with unchecked output growth.

### 11.3 Resource-fork compression

Resource-fork-backed payloads involve Apple resource framing and a chunk table, **not merely one zlib stream occupying the entire fork**. Chunks are typically up to **64 KiB uncompressed**. Resource-map offsets, compressed-block lengths and fragment offsets need rigorous validation. For resource types 4, 8, 12, first read the resource fork via normal extent mapping; then decode the specific compression resource format, bounded by the advertised uncompressed size and compressed fork size. A handy interoperability reference is the documentation and implementation links [D1,C2]; the uncompressed output, not internal compressed block layout, is the cross-tool oracle.

**Layering:** (1) parse catalog and forks; (2) resolve physical xattrs; (3) inspect compression flag and `com.apple.decmpfs`; (4) present `logical_data_stream` = decompressed data, while also providing `raw_data_fork`, `raw_resource_fork` and `raw_decmpfs_xattr`. This avoids a circular dependency between fork reader and decompressor. If compression metadata is corrupt, do not silently return an empty or partial logical file as success.

**Writer recommendation:** Stage 1 creates only uncompressed content (`UF_COMPRESSED` unset, no `com.apple.decmpfs`); stage 3 may preserve a raw valid compressed representation or add compression with byte-for-byte fixtures. Never set a compression flag without a matching attribute and valid encoding. If a user submits a compressed representation as an opaque copy, require a validate-and-preserve mode that preserves resource fork and catalog flag together.

## 12. Journaling and crash consistency

HFS+ **journaling is optional** and records metadata operations; ordinary file data is not guaranteed by HFS+ journal recovery. Journaling should be separated from the generic catalog/extent reader. A volume that says it is cleanly unmounted may still have an on-disk journal; conversely, a journaled volume may need recovery or have an inconsistent unmount state. [A1]

### 12.1 Journal flag and JournalInfoBlock

Volume header `attributes` includes `0x00002000` (journaled); volume header `journalInfoBlock` is the **allocation-block index** of a JournalInfoBlock (JIB). The JIB begins at byte 0 of that physical allocation block:

| Offset | Width | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `flags` | `0x1` journal inside FS, `0x2` on other device, `0x4` needs initialization |
| 4 | 32 | `device_signature[8]` | External journal device identity / reserved in local-journal cases |
| 36 | 8 | `offset` | Byte offset of journal header; **relative to volume base if local** |
| 44 | 8 | `size` | Entire journal area length in bytes |
| 52 | 128 | reserved[32] | Zero for writer; reader ignores |

JIB size = **180 bytes**, and this struct must fit inside its allocation block. For a local journal, `offset + size` must be within the volume and its allocation blocks must be allocated; a separate external journal device cannot be safely replayed by an image-only library unless its bytes are separately available. `flags&0x4` signifies that no initialized transactions should be read as valid. [A1]

### 12.2 Journal header and ring buffer

Starting at the JIB `offset`, the journal's 44-byte minimum header (multi-byte values in the journal's own indicated endianness) is:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | `magic = 0x4A4E4C78` |
| 4 | 4 | `endian = 0x12345678` |
| 8 | 8 | `start` relative to beginning of journal |
| 16 | 8 | `end` relative to beginning of journal |
| 24 | 8 | `size` including journal header and buffer |
| 32 | 4 | `blhdr_size` block-list-header size |
| 36 | 4 | `checksum` |
| 40 | 4 | `jhdr_size` header/sector alignment size |

The transaction buffer is circular. **`start == end` indicates no transactions to replay**, after validating header/checksum. A nonempty buffer describes metadata block rewrites using block-list headers and block-info entries (`bnum:u64`, `bsize:u32`, `next:u32`). Block addresses are **sector** addresses, not HFS allocation-block indices, and a sentinel block number of all ones means skip. `blhdr_size` and `jhdr_size` come from the on-disk header, and sector granularity may differ from allocation-block size. Checksums are checked with the checksum field treated as zero. [A1]

**Do not implement replay by blindly writing to the input image.** Correct recovery requires transaction boundary/checksum validation, circular iteration, order, a bounded block map, and overflow checks; maintain an in-memory **overlay of journal-replayed sectors** so fsimg can extract a consistent logical image without editing the source. Source material contains subtleties about the special initial `block_info` entry, transaction chaining, checksum and block-count conventions. Defer until independently tested with intentionally interrupted journaled fixtures. `fsck_hfs` is a useful oracle for the journal state. [A1]

### 12.3 Explicit reader policy

| On-disk condition | Default recommended fsimg behavior |
|---|---|
| Nonjournaled, clean flag | Parse normally and validate metadata |
| Journaled, initialized, cleanly unmounted, empty journal | Permit extraction; annotate journal feature |
| Journaled, unclean and/or nonempty journal | **Fail strict mode**; permissive `--ignore-journal` can list base-state with a warning; later `--replay-journal` overlay |
| Journaled but JIB missing/corrupt | Error, or explicitly advertised salvage only |
| External journal | Unsupported without separately supplied device image |
| JIB needs initialization | Handle as a special journal-uninitialized state, not valid replay log |

A nonjournaled writer avoids all journal complexity: clear journal bit, zero the JIB field, set clean/unmounted flag, and write final metadata before headers. *Write ordering of a freshly generated image is not equivalent to mount-time transactional crash consistency*; build into a temporary output and atomically rename on successful validation.

## 13. HFS wrapper and partition variants

### 13.1 Detect a legacy HFS wrapper

HFS+ is frequently raw, but may be embedded as a contiguous virtual HFS+ volume inside an old-style HFS filesystem. Detect an HFS MDB (`'BD'`, signature `0x4244`) at **wrapper base+1024**, then inspect the MDB embedded-signature field at MDB offset **`0x7C`**. An embedded HFS+ volume is signaled by `0x482B` (`H+`); field at **`0x7E`** gives a pair of unsigned 16-bit numbers `(embedStartBlock, embedBlockCount)` in units of the **outer HFS** allocation-block size. [A1]

To find the inner HFS+ volume:

```
innerBase = outerBase + 512 * MDB.drAlBlSt
                      + MDB.drEmbedExtent.startBlock * MDB.drAlBlkSiz
innerBytes = MDB.drEmbedExtent.blockCount * MDB.drAlBlkSiz
read HFS+ primary volume header at innerBase + 1024
```

Here `drAlBlSt` is the outer HFS allocation region start, expressed in **512-byte physical blocks**, and `drAlBlkSiz` is outer HFS allocation block size **in bytes**. The inner HFS+ volume length for alternate-header calculation is `innerBytes`, not the enclosing image length. Require the entire embedded extent to be inside the outer volume, block-aligned and nonzero; require the inner signature/version to validate. The outer wrapper bitmap marks these allocation blocks occupied and its bad-block pseudo-file also protects the space. [A1]

**Warning:** a wrapper is a complete outer HFS filesystem. Creating one that works with old classic Mac systems requires outer MDB, extents/catalog trees, bitmap, bad-block tracking and boot-support conventions. This is **not** the same as just preceding an HFS+ image with an MDB. `fsimg` should initially *read embedded HFS+ volumes* but defer wrapper creation until legacy HFS support exists.

### 13.2 APM/GPT, raw partitions, DMG

- **GPT/APM:** partition discovery belongs to the shared fsimg image/container layer. Invoke the HFS+ driver with `(volumeBase, volumeLength)`. The HFS+ header and allocation-block numbering are relative to that segment.
- **DMG:** a `.dmg` file may be raw-ish uncompressed UDIF or compressed UDIF, or may contain an Apple partition map. `hdiutil convert -format UDRW` on macOS or a compatible `dmg2img` tool (if available) can produce a raw disk payload; parse any nested partitions after conversion. Do not assume an arbitrary DMG converts into a raw HFS+ *volume* as opposed to a raw *partitioned disk*.
- **Unwrapped HFSX:** `HX` signature directly at selected volume base+1024; HFSX does not use the legacy HFS wrapper convention.
- **Cross-platform:** Linux mount implementations and macOS mount utilities often accept device/loop volumes rather than arbitrary image slices. Use explicit offsets/loop devices or generate a raw standalone volume for interoperability tests.

## 14. Implementation architecture for a create/extract-only fsimg backend

### 14.1 Narrow module boundaries

Prefer components that can be unit-tested on in-memory byte spans, independently from IO and your top-level archive-style API:

| Proposed module | Responsibilities | Dependencies |
|---|---|---|
| `hfs_probe` | Raw H+/HX vs HFS wrapper, volume base/length, signatures | Generic image/partition reader |
| `hfs_io` | Bounds-checked little/big-endian reads, fixed-size writes, extent logical->physical mapping | `fsimg_source`, `fsimg_sink` |
| `hfs_volume` | Headers, feature flags, timestamps, special-file descriptors | `hfs_io` |
| `hfs_btree` | Node framing, lazy leaf/index/map reads, tree cursor, sorted record iterator, bulk builder | `hfs_io`, comparator injection |
| `hfs_unicode` | Stable Unicode decomposition, comparison, UTF-16BE and host-path translation | Frozen versioned tables |
| `hfs_catalog` | Primary+thread records, path lookup, CNIDs, Finder/BSD metadata, folder counts | `hfs_btree`, `hfs_unicode` |
| `hfs_extents` | Fork reads and overflow-tree integration; reserved/overlap validation | `hfs_btree`, `hfs_io` |
| `hfs_attributes` | Modern xattr B-tree and virtual xattr projection | `hfs_btree`, `hfs_extents` |
| `hfs_special` | Symlinks, hardlinks, hidden dirs, `decmpfs` metadata decoding | Catalog, attributes, fork reader |
| `hfs_verify` | Full consistency graph, allocation set, defensive-parser diagnostics | All read modules |
| `hfs_create` | CNID/layout planner, allocator, B-tree builder, deterministic writer | Shared metadata model, builders |

**API suggestion** (conceptual only, not a binding commitment):

- `probe(source, offset?, length?) -> volume descriptor`, including `flavor` (`HFS_PLUS`, `HFSX_FOLDED`, `HFSX_BINARY`), blockSize, volumeBytes, flags, needsJournalReplay.
- `walk(visitor, options)` supplies normalized path, raw UTF-16 units, CNID, type, timestamps, Finder metadata, POSIX info, xattrs, and independent data/resource forks; do not materialize large files.
- `open_fork(cnid, kind)` supports seek/read; `open_contents(cnid)` resolves decompression/hardlinks transparently, and `open_raw_fork` bypasses transformation.
- `create(entries, options, sink)` writes new images only; entries may be streamed into a *manifest/spool* if the source cannot be enumerated twice. A seekable sink simplifies final headers; a temporary file also makes it safe for nonseekable callers.
- `verify(source, depth)` emits structured severity, byte ranges, object identity and invariant ID; use one validation engine to test both reader and writer output.

Maintain a strict division between **on-disk namespace** and **exported host representation**. The generic fsimg layer should have explicit metadata for resource forks and xattrs; packing resource bytes into AppleDouble `._name` files is a **lossy export policy**, not how HFS+ physically stores the fork.

### 14.2 Reader bootstrapping / avoiding recursion

1. Establish `(volumeBase,volumeBytes)`, decide whether it is direct HFS+/HFSX or embedded in HFS. Parse the primary header; corroborate alternate header if available.
2. Parse the volume allocation geometry. Initialize the **extents B-tree reader first** using its inline header fork extents; its own allocated fork should not require overflow lookup.
3. Initialize generic fork reader. It may now resolve overflow extents for catalog/attrs and all ordinary objects.
4. Open catalog tree node 0 and verify nodeSize/key features; inspect HFSX key comparator **before** attempting a catalog binary search.
5. Open attributes tree if present, but permit read-only `no attributes` images.
6. Enumerate catalog through sorted leaves or search from root; resolve xattrs/compression only after core byte-stream logic works. Implement cycle guards and caches keyed by `(treeCNID,nodeNo)` and `(fileCNID,forkType)`.
7. If journaled and uncertain, enforce section 12 policy *before trusting* base-state metadata as consistent.

**A byte stream abstraction is especially valuable here:** a B-tree node may cross an HFS+ allocation-block boundary, and a B-tree file itself may be physically fragmented. An algorithm that assumes B-tree node number 1 is at `firstExtentStart + nodeSize` in the raw image will fail legitimate images.

### 14.3 Writer: exact build and placement sequence

A feasible, *without in-place editing*, image-builder algorithm:

**Pass A — normalize and plan namespace:**

1. Select output type HFS+/HFSX and comparison mode, volume length, allocation block size, tree node sizes, locale-independent timestamp policy, Finder default bytes, journal off.
2. Traverse input manifest; normalize each name once; reject empty/invalid user names, length >255 UTF-16 units and duplicate comparator-equal sibling names. Validate unsupported record types and namespace collisions *before writing*.
3. Assign CNIDs deterministically beginning at 16; CNID2 is root. Reserve CNID3..8 and other reserved IDs. Every ordinary primary record and its thread refer to the same CNID; nextCatalogID is greater than every assigned CNID. Source hardlink groups require a separate plan.
4. Calculate per-folder immediate child count (`valence`) and direct folder count (`folderCount` when applicable). Calculate volume `fileCount` and `folderCount` (exclude root, special files, and catalog threads). Prepare catalog primary/thread records with **fork descriptors not yet filled**.
5. Gather files' data forks and resource forks and all large xattr values. Determine byte counts and the minimum physical allocation needed. In first milestone, allocate each fork in a single contiguous run. For test mode accept a preplanned intentional fragmentation map.

**Pass B — determine metadata sizes independently of their addresses:**

6. Construct xattr-key/value records or mark attributes file absent. For fork-backed xattrs, initial fork descriptors remain placeholders until allocated.
7. Estimate catalog and extents tree sizes. The number of catalog records is known (normally 2× primary catalog objects). Catalog record *lengths* do not change with physical extent locations; therefore they can be packed once and their fork descriptor bytes patched in an in-memory representation before final serialization.
8. Choose catalog/attribute/extents node sizes and pack enough leaf/index nodes. Compute map-node overhead from total node count. Preallocate optional slack if desired; **all unallocated B-tree nodes are zeroed and marked free in the B-tree node bitmap**, though their containing HFS allocation blocks remain reserved.
9. The extents tree length depends on fragmentation. For a simple contiguous writer, no user overflow records exist; its size is just one empty header node. For deliberate fragmentation, calculate all overflow records from planned fork fragmentation before building the tree. Avoid using extents overflow records for the extents tree itself.
10. The bitmap's minimum `logicalSize` is `ceil(totalBlocks/8)`, rounded up to allocation blocks for physical occupancy. You may reserve an additional clump. The volume's alternative header and reserved zones must be marked used independently from the metadata files.

**Pass C — allocate physical blocks:**

11. Initialize an interval allocator with `[0,volumeBlocks)` and mark reserved/boot/header/alternate-header blocks as unavailable, using actual byte-overlap checks rather than assuming they are block 0 and the final block under every geometry.
12. Reserve fixed-size metadata files (bitmap, catalog, extents, optional attributes and startup), then reserve data/resource/xattr content. Or allocate user forks first and metadata afterward; either is valid as long as all extents are known before B-trees are serialized.
13. Fill each fork descriptor with `logicalSize`, `totalBlocks`, `extents[8]`, and `clumpSize`. Place continuation records for forks with >8 extents into extents tree or attributes tree accordingly. If this changes extents-tree size, **iterate planning/allocation to a fixed point**, or reserve a safe metadata-size bound before physical placement. On a fresh contiguous image with <=8 extents per fork, no circularity occurs.
14. Materialize B-tree keyed leaf records and index nodes with exact comparators, node maps, header record and offsets. Ensure every special file's fork in the volume header covers the entire B-tree logical length.
15. Produce the volume allocation bitmap from final ownership, mark partial reserved areas and unused tail bits consistently, compute `freeBlocks` and `nextAllocation` (a search hint, not “first block after all data” requirement).

**Pass D — serialize and finalize:**

16. Write all file/resource/xattr content to assigned blocks, zero-pad slack. Write catalog/extents/attributes B-trees and allocation bitmap. Optionally zero the remaining unallocated image bytes for a deterministic dense image (sparse backing can avoid physically storing zero holes).
17. Populate 512-byte primary header fields, including CNID ceiling, date policy, clean bit, counts, encoding bitmap, physical metadata fork extents, and data/rsrc clump sizes.
18. Write identical, coherent primary header at `base+1024` and alternate header at `base+volumeBytes−1024`; ensure nothing afterward overwrites these bytes. If the output is partitioned, the shared image layer writes GPT/APM before or after without modifying the volume base offset.
19. Run **independent validator** over the output and, where practical, external `fsck_hfs -n`/`fsck.hfsplus -n`; write to a temporary path then atomically promote only if all required checks pass.

*Why bulk-building is enough:* it eliminates catalog/index node splitting, free-space coalescing, journal transactions, deletion and tree rebalancing. It does **not** eliminate metadata correctness, comparator bugs, overflow extent chains or bitmap accounting.

### 14.4 Capacity and metadata-sizing formulas

Let `B` = allocation block bytes, `V` = exact volume bytes, `N=floor(V/B)` (subject to reserved-area constraints), `M` = B-tree node bytes, `F` = number of ordinary catalog primary records plus root, and `A` = attribute record count.

- `bitmapBytes = ceil(N / 8)`, bitmap allocated blocks = `ceil(bitmapBytes / B)`.
- `forkBlocks = ceil(forkLogicalBytes/B)` (can exceed this with optional clumps).
- `treeLogicalBytes = totalTreeNodes*M`; tree allocated blocks >= `ceil(treeLogicalBytes/B)`.
- A leaf with `R` records fits when `14 + sum(evenPaddedRecordSizes) + 2*(R+1) <= M`.
- For an index level, `R` keys each hold a child node (`4 bytes`) and the key's serialized allocation. A non-root index node should normally have at least two child records; if packing produces a singleton at the last node, rebalance children across nodes so it has two or more. Root can be a leaf or index with at least two children.
- An 8192-byte B-tree header node holds `(8192−256)*8 = 63,488` allocation bits; each additional map node holds `(8192−20)*8 = 65,376` node bits. Map-node count follows from `totalTreeNodes`; solve via a small fixed-point computation.
- If `bytesReserved + bytesSpecial + bytesForks > allocatableVolumeBytes`, fail `ENOSPC` **before** writing. Include B-tree node maps, fork clumps, overflow records, bitmap and both headers in the estimate.

**Metadata size example (illustrative, not a canonical disk layout):** A 128 MiB volume with 4096-byte allocation blocks contains 32,768 blocks. Its bitmap needs 4096 logical bytes, exactly one 4096-byte allocation block. An 8192-byte B-tree header occupies two HFS allocation blocks even when its tree has no leaves. The first and last 4096-byte allocation blocks are reserved due to volume/alternate-header overlap. This demonstrates why `volume bytes == user storage bytes + tree bytes` is incorrect without block rounding/reserved accounting.

## 15. fsimg validator and formalized invariants

This section is a **structured specification for property-based tests, libFuzzer, and safe parsing**, not merely a “run fsck” instruction. Recommended severity tags: **fatal** (unsafe read or unrecoverable invalid structure), **invalid** (format-violation, safe to inspect), **incomplete/unsupported** (valid but unsupported feature), **warning** (noncritical inconsistency or historical variant), **info** (header statistics).

### 15.1 Header and address checks

| ID | Property | Mutation to test |
|---|---|---|
| H-01 | Signature/version supported and paired | `H+` + version 5; random signature |
| H-02 | 512-byte header fully inside selected volume | Truncated before offset1536 |
| H-03 | blockSize >=512 and power-of-two | 0, 1, 513, `2^31` |
| H-04 | `totalBlocks` and special-file spans fit selected physical image | `UINT32_MAX`, overflow in multiplication |
| H-05 | `freeBlocks<=totalBlocks`, `nextAllocation` in range | `freeBlocks=N+1` |
| H-06 | Primary and alternate headers coherent or recovery reported | Corrupt one copy only |
| H-07 | Required special forks present and structurally reasonable | Catalog zero-sized or inaccessible |
| H-08 | No reserved/header-overlapping allocation claimed by user data | Extent at block0 or final header block |
| H-09 | Clean/journal attributes and JIB are coherent | Journal flag set, no valid JIB |
| H-10 | `nextCatalogID` beyond assigned ordinary object IDs | Duplicate/reused high CNID |

### 15.2 Fork and allocation checks

| ID | Property | Mutation to test |
|---|---|---|
| F-01 | `logicalSize <= totalBlocks*B`, arithmetic checked | Huge logicalSize, zero blocks |
| F-02 | Initial and overflow extents sum exactly to fork totalBlocks | Edit final extent block count |
| F-03 | Used extent ranges are within `[0,totalBlocks)` | Start near 2^32−1, count >1 |
| F-04 | Used extent descriptors precede empty ones | `(0,0)` gap then nonempty |
| F-05 | Overflow key's logical start block is exact cumulative block count | Missing/duplicate overflow record |
| F-06 | No two independent forks own same allocation block | Force resource and data forks to overlap |
| F-07 | Bitmap equals reconstructed ownership (modulo allowed metadata/bad blocks) | Flip allocated bit; flip free bit |
| F-08 | Padding bits past `totalBlocks` are zero | Set final byte high/low tail bit |
| F-09 | Bitmap logical size enough for all block bits | Trim bitmap fork by one byte |
| F-10 | Special files can be accessed through overflow only where legal | Fragment extents tree into >8 extents |

### 15.3 B-tree node/record checks

| ID | Property | Mutation to test |
|---|---|---|
| T-01 | Node0 header node; descriptor, record offsets and 106B record fit | Fuzz nodeSize and offset footer |
| T-02 | Every node `kind`, `height`, `numRecords` plausible | Index node with leaf height1 |
| T-03 | Offsets monotonically increase; `N+1` positions fit | Overlapping offsets; out-of-node read |
| T-04 | Key length/padded payload fit individual record | Max key with 1-byte payload |
| T-05 | All in-node and across-sibling keys strictly ascending under correct comparator | Equal keys, descending key |
| T-06 | Index child node referenced and height decreases by exactly one | Child 0/header node/cycle |
| T-07 | Index separator equals minimum key in child's subtree | Perturb separator by 1 code unit |
| T-08 | Leaf `fLink`/`bLink` reciprocal, no cycles, endpoints agree with header | Circular list; wrong lastLeaf |
| T-09 | `leafRecords` equals total leaf entries; `treeDepth` root height | Off-by-one count |
| T-10 | Tree node maps cover and mark every reachable node, map chain valid | Unmarked active leaf; cyclic map chain |
| T-11 | Fork logical size compatible with `totalNodes*nodeSize` | Truncate in middle of a node |
| T-12 | Variable/fixed index key layout consistent with header flags | Toggle `0x04` on extents tree |

### 15.4 Catalog/attribute/semantic checks

| ID | Property | Mutation to test |
|---|---|---|
| C-01 | Valid catalog primary and thread type/record sizes | Unknown record type; truncated file record |
| C-02 | Every file/folder primary has valid unique CNID and corresponding correct thread | Drop thread; wrong parent in thread |
| C-03 | Root=CNID2, parent=CNID1; every reachable parent is folder | Parent missing or parent is file |
| C-04 | No ancestor cycles and no unintended duplicate physical paths | Directory parent points into child |
| C-05 | `valence`, folderCount, volume file/folder counts match | Alter counts independently |
| C-06 | Catalog names satisfy storage syntax and comparator uniqueness | Two compare-equal sibling names |
| C-07 | xattr keys are sorted and CNIDs resolve, inline data length within leaf | Duplicate attr, forged size |
| C-08 | Compressed object flags, attribute, resource fork and decoded length consistent | `UF_COMPRESSED` without xattr |
| C-09 | Hardlink references resolve to proper indirect-node content | `iNode` missing, ref=0 |
| C-10 | Symlink content extracted without path traversal | Absolute and `../../` target |
| C-11 | HFSX comparator type obeyed, incl binary-vs-folded name collision behavior | Flip `0xBC`/`0xCF` |
| C-12 | Special Finder bytes not lost during round trip | Custom file creator/type/flags |

**Exact accept/reject boundaries:** Preserve raw bytes and return structured errors; allow an explicitly named `recovery`/`salvage` mode to continue after *recoverable* node inconsistencies. Never let salvage mode waive memory-safety or destination-path constraints.

### 15.5 Fuzz implementation tactics

- **Property-based generators:** Generate semantically valid trees/forks/manifests from small bounded integers, then mutate one structural constraint at a time. Useful dimensions: blocks (0,1,2,7,8,9,65535), B-tree node counts near bitmap boundary, name length 0/1/254/255 UTF-16 units, weird Unicode, fork length just below/exactly/above block boundary, data/resource fork independently empty or nonempty.
- **Model oracle:** An *independent, simple reference implementation* in Ruby/Python can build in-memory sorted key-value records and expected logical path/fork content (not another on-disk parser). Compare fsimg's result to this model. Test catalog comparator against a frozen golden-name corpus.
- **Mutation fuzzing:** corpus of `hdiutil`/`mkfs.hfsplus` images; mutate headers, B-tree nodes, extents and xattr length fields. Build tiny minimal valid images for libFuzzer or use `fuzzed-data-provider` to fake the image read source. Start with per-module fuzz targets to avoid gigabyte-scale full images.
- **LibFuzzer targets:** `parse_volume_header`, `parse_btree_node`, `parse_catalog_record`, `parse_attr_record`, `parse_extent_record`, `compare_hfs_names`, `parse_decmpfs_header`, `parse_journal_header`, and `full_extract_memimage` (small corpus). Enable ASan+UBSan and a time/iteration budget; do not expose host filesystem writes in the fuzz harness.
- **Differential tests:** Generated *valid* image must pass `fsck_hfs -n`, macOS mount/list/copy, hfsfuse reader, and ideally a forensic parser. Generated *invalid* image must not crash any in-process parser; independent tools may legitimately reject it with different diagnostics.
- **Round-trip:** `manifest -> fsimg create -> fsimg extract -> manifest comparison` plus `Apple/macOS create -> fsimg extract -> manifest comparison`, and `fsimg create -> native macOS read/write -> fsimg extract`. Metadata comparisons must consider normalization and time resolution.
- **Delta debugging:** When a generated image fails an oracle, minimize manifest first (subtree/object removal), then byte-level diff between a minimal passing and failing case; store precise image generator seed and mutations.

A full “formal legality oracle” is most practical as a small executable rule table rather than SMT solving: represent min/max/exact sizes, enum values, flags, arithmetic relations, graph constraints and referential constraints. Most checks above are linear scans, sorting, and local assertions.

## 16. Actual interoperability tools and commands (read **and** write)

Commands below are **proposed test recipes, not commands executed during this research**. Confirm installed versions and flags via each tool's `--help`/`man` on your CI platform. Never mount malformed/fuzzed images read/write on a production host; use controlled VMs, copies, and offline parsers.

### 16.1 Capability matrix

| Tool | Host | Format/create | Read/extract | Write/mutate image | Check/repair | Comments |
|---|---|---|---|---|---|---|
| **`hdiutil` + native macOS HFS driver** | macOS | **Yes**, HFS+, HFSX, journaled variants | **Yes**, via attach/mount | **Yes**, via mounted filesystem | Via `diskutil` / `fsck_hfs` | Strongest Apple-native oracle [E1] |
| **`newfs_hfs`** | macOS | **Yes** | No | Initializes volume only | No | Adjustable block/tree sizes, journal [E2] |
| **`fsck_hfs`** | macOS | No | Metadata diagnostic only | Repair mode only | **Yes** | Use `-n` to avoid modifications [E3] |
| **`mkfs.hfsplus` (`hfsprogs`)** | Linux | **Yes** | No | Initializes volume only | No | Apple-derived tool port, separate APSL licensing [E4] |
| **`fsck.hfsplus` (`hfsprogs`)** | Linux | No | Metadata diagnostic only | Repair mode only | **Yes** | Use `-n` [E5] |
| **Linux kernel `hfsplus` mount** | Linux | No | **Yes** | **Potentially yes**, constrained/limited | No | RO default for test; journaled-write issues [C3] |
| **`hfsfuse`** | Linux/BSD/other FUSE | No | **Yes** | **No** | Not full fsck | Good independent tree/fork reader [C1] |
| **`hfsdump`, `hfstar`** | Linux/BSD/Windows builds | No | **Yes** | **No** | Header/record inspection only | `hfsfuse` standalone tools [C1] |
| **Sleuth Kit `fsstat`, `fls`, `istat`, `icat`** | Multiple | No | **Yes** | **No** | Inspection, not filesystem repair | Distinct forensic parser [C2] |
| **`libfshfs` / tools** | Multiple | No | **Yes** | **No** | Forensic inspection | LGPL/GPL library/tool licensing [C5] |
| **HFSExplorer / `unhfs`** | Java/cross-platform | No | **Yes** | **No** | No | GPLv3+ forensic extraction [C4] |
| **Debian `hfsplus` (`hpmount`, `hpls`, `hpcopy`, `hpmkdir`, `hprm`, `hpfsck`)** | Linux | No (uses other formatter) | **Yes** | **Experimental**, directory creation/removal; avoid on originals | Basic `hpfsck` | Non-kernel independent implementation; `hpcopy` man page documents copying *from* HFS+ [C7] |
| **NetBSD HFS kernel reader** | NetBSD | No | **Yes** | Not a writer/oracle | No full standalone fsck | BSD 2-clause code reference [B1,B2] |
| **Rust `hfsplus` crate** | Cross-platform | No | **Yes** | No | Library parser tests | Recent implementation with HFSX, xattrs/compression support [C6] |
| **JetBrains `go-dmg-writer`** | Cross-platform | **Yes** | No | Creates only | No | Clean-room, single-pass HFS+ volume plus optional UDIF/GPT container; Apache-2.0, **reference only** [C9] |

**Avoid false friends:** `hfsutils` (without `plus`) generally targets **HFS Classic**, *not* HFS+. `mkfs.hfs` from `hfsprogs` may be an alias for `mkfs.hfsplus`, while an unrelated `hformat` is likely a classic-HFS formatter. Always verify the output signature at +1024.

### 16.2 macOS — produce reference images of all four variants

The `-layout NONE` option avoids creating a partition map. `UDRW` means writable disk image; it's a container variant and may have a UDIF trailer — obtain a guaranteed bare filesystem byte stream by attaching with `-nomount` and copying the resulting virtual device when necessary. Do **not** equate `.dmg` file length with HFS+ volume length without inspecting the container. [E1]

```sh
# Run on macOS. Keep each fixture on a disposable test path.
mkdir -p /tmp/fsimg-hfsplus-reference
cd /tmp/fsimg-hfsplus-reference

hdiutil create -size 128m -layout NONE -format UDRW \
  -fs HFS+ -volname FSIMG-PLUS plus.dmg
hdiutil create -size 128m -layout NONE -format UDRW \
  -fs HFSX -volname FSIMG-CASE case.dmg
hdiutil create -size 128m -layout NONE -format UDRW \
  -fs HFS+J -volname FSIMG-JOURNAL journal.dmg
hdiutil create -size 128m -layout NONE -format UDRW \
  -fs JHFS+X -volname FSIMG-JOURNAL-CASE journal-case.dmg

# Inspect and attach for controlled read/write fixture creation.
hdiutil imageinfo plus.dmg
hdiutil attach plus.dmg
# Inspect returned /dev/diskN and /Volumes/FSIMG-PLUS.
# After adding files, unmount cleanly and detach the returned disk.
# hdiutil detach /dev/diskN
```

Examples while mounted (mount path and permissions may vary):

```sh
VOL='/Volumes/FSIMG-PLUS'
mkdir -p "$VOL/dirs/level2"
printf 'hello from macOS\n' > "$VOL/dirs/level2/plain.txt"
dd if=/dev/urandom of="$VOL/random.bin" bs=4096 count=7
ln -s 'dirs/level2/plain.txt' "$VOL/symlink"
ln "$VOL/random.bin" "$VOL/random-hardlink"
xattr -w com.example.fsimg 'hello-xattr' "$VOL/random.bin"
# Create Finder info / resource fork via macOS API or named-fork APIs if needed.
# Ensure the test filesystem supports all desired features before asserting.
stat "$VOL/random.bin"
ls -lO "$VOL"
ls -le@ "$VOL"
xattr -l "$VOL/random.bin"
sync
```

For case-sensitive test attach the HFSX fixture and verify `Name` and `name` can coexist. For case-insensitive HFS+ compare names with decomposed Unicode and ensure lookup agrees with native macOS behavior. For compressed-file fixture, [E7] documents `ditto --hfsCompression`:

```sh
# Example: source directory on regular host filesystem, destination HFS+.
mkdir -p /tmp/fsimg-hfsplus-reference/compress-src
python3 - <<'PY'
from pathlib import Path
Path('/tmp/fsimg-hfsplus-reference/compress-src/repetitive.txt').write_bytes(
    b'compressible-text-' * 12000)
PY
# Target should be a mounted, writable HFS+ reference image.
ditto --hfsCompression /tmp/fsimg-hfsplus-reference/compress-src \
  /Volumes/FSIMG-PLUS/compress-fixture
# Inspect whether the actual file was compressed (not every file is).
ls -lO@ /Volumes/FSIMG-PLUS/compress-fixture/repetitive.txt
```

External validation of *your* image (do not point `fsck_hfs` at a mounted writable device):

```sh
# Attach without mounting; record the /dev/diskN returned by the tool.
hdiutil attach -nomount -readonly ./fsimg-output.img
# Use the appropriate raw device reported (not a guessed number).
sudo fsck_hfs -n /dev/rdiskN
# For deeper diagnostic output: sudo fsck_hfs -n -d /dev/rdiskN
hdiutil detach /dev/diskN
```

Native macOS can also mount a generated image read/write. For true write validation, do it **only on a copy**, write/rename/delete a set of files, cleanly detach, run `fsimg extract`, and compare. Expect the OS to create private `.fseventsd`/`.Spotlight` metadata on some configurations; filter or account for these separately in path comparisons.

### 16.3 Linux — reference images with hfsprogs and read/write via kernel

`hfsprogs` commonly provides `mkfs.hfsplus` and `fsck.hfsplus`; the exact program aliases vary by distribution. Formatting a *regular file* works on many builds, but some require a loop device; test `mkfs.hfsplus --help`. The examples below assume file-based formatting works:

```sh
# Debian/Ubuntu; do not run against real disks.
sudo apt-get install hfsprogs hfsplus
truncate -s 128M /tmp/fsimg-hfsplus-ref.img
mkfs.hfsplus -v FSIMG-REF -b 4096 /tmp/fsimg-hfsplus-ref.img
fsck.hfsplus -n /tmp/fsimg-hfsplus-ref.img

truncate -s 128M /tmp/fsimg-hfsx-ref.img
mkfs.hfsplus -s -v FSIMG-CASE -b 4096 /tmp/fsimg-hfsx-ref.img
fsck.hfsplus -n /tmp/fsimg-hfsx-ref.img

# Optional journal/reference node-size variation:
truncate -s 256M /tmp/fsimg-hfsplus-j.img
mkfs.hfsplus -J -n c=8192,e=4096 -v FSIMG-J /tmp/fsimg-hfsplus-j.img
```

To read the image using Linux kernel (prefer read-only):

```sh
sudo mkdir -p /mnt/fsimg-hfsplus
sudo mount -t hfsplus -o loop,ro /tmp/fsimg-hfsplus-ref.img /mnt/fsimg-hfsplus
find /mnt/fsimg-hfsplus -mindepth 1 -maxdepth 3 -print
sudo umount /mnt/fsimg-hfsplus
```

For limited **write** testing, use an unjournaled image copied from an fsimg output, and mount only within a disposable VM:

```sh
cp fsimg-output.img /tmp/fsimg-write-test.img
sudo mount -t hfsplus -o loop,rw /tmp/fsimg-write-test.img /mnt/fsimg-hfsplus
# Perform writes to /mnt/fsimg-hfsplus, then unmount and run fsck on the copy.
sudo umount /mnt/fsimg-hfsplus
fsck.hfsplus -n /tmp/fsimg-write-test.img
```

**Linux kernel caveat:** journaled HFS+ has historically not supported normal RW mounts in the Linux HFS+ driver; manually forcing RW on dirty/journaled volumes risks data loss. Do not use Linux `mount -o force,rw` as a compatibility criterion or on valuable images.

### 16.4 hfsfuse / hfsdump / hfstar — independent, read-only oracle

`hfsfuse` includes two **non-FUSE** utilities useful in CI. The project's README documents these exact modes [C1]. **Run the path-specific examples against a populated image** containing `/dirs/level2/plain.txt`, `/random.bin`, and a file with a resource fork (substitute paths to match your own fixture); a freshly formatted blank image will not have these files:

```sh
# Built from hfsfuse upstream; hfsdump/hfstar do not require FUSE mount.
hfsdump /tmp/fsimg-hfsplus-ref.img
hfsdump /tmp/fsimg-hfsplus-ref.img stat /dirs/level2/plain.txt
hfsdump /tmp/fsimg-hfsplus-ref.img read /dirs/level2/plain.txt > /tmp/hfs-dumped.txt
hfsdump /tmp/fsimg-hfsplus-ref.img xattr /random.bin
hfsdump /tmp/fsimg-hfsplus-ref.img xattr /random.bin com.example.fsimg
hfsdump /tmp/fsimg-hfsplus-ref.img read /file-with-resource/rsrc > /tmp/rsrc.bin

# Archive to tar via libarchive; inspect entries and content independently.
hfstar /tmp/fsimg-hfsplus-ref.img /tmp/fsimg-hfstar.tar
tar -tf /tmp/fsimg-hfstar.tar

# Optional read-only FUSE access (mount point created beforehand).
hfsfuse /tmp/fsimg-hfsplus-ref.img /tmp/fsimg-hfsfuse-mnt
```

For dirty journals, `hfsfuse` offers `--force`; this bypass is not journal replay and must not be used as proof of a consistent filesystem. The `hfstar` output is a useful archive-level cross-check, but tar itself may not preserve every HFS-specific metadata field, resource fork or directory hardlink topology under every format/options combination.

### 16.5 Sleuth Kit — independent forensic traversal

TSK reads HFS/HFS+ metadata using different code paths from macOS and provides valuable disagreement signals [C2]:

```sh
fsstat -f hfs /tmp/fsimg-hfsplus-ref.img
fls -f hfs /tmp/fsimg-hfsplus-ref.img
fls -f hfs -r /tmp/fsimg-hfsplus-ref.img
# Replace CNID with the inode/CNID printed by fls.
istat -f hfs /tmp/fsimg-hfsplus-ref.img CNID
icat -f hfs /tmp/fsimg-hfsplus-ref.img CNID > /tmp/tsk-file.bin
```

When partitions are involved, identify the filesystem's sector offset and pass TSK's `-o` option in **sectors**, not bytes. The `-f hfs` short name and `istat`/`icat` inode conventions should be verified against installed version. On an HFS+ image containing data forks and resource forks, compare the default `icat` stream against the *logical data stream* expected by TSK; use dedicated raw-resource-fork tests for streams it does not expose conveniently.

### 16.6 Debian `hfsplus` (`hp*` commands) — second Unix userland implementation

Unlike `hfsutils` (legacy HFS), Debian's distinct `hfsplus` package works with HFS+. Its `hpmount` accepts a regular file and is RW by default; `-r` selects RO, and it writes a `.hfsplusvolume` state file in the user's home directory. [C7]

```sh
# Read-only inspection of a local image:
hpmount -r /tmp/fsimg-hfsplus-ref.img
hpls
hpfsck -v /tmp/fsimg-hfsplus-ref.img
hpcopy -r some-file /tmp/copied-file
hpumount

# On a THROWAWAY copy, hpmount without -r allows experimental writes.
cp /tmp/fsimg-hfsplus-ref.img /tmp/hp-rw-experiment.img
hpmount /tmp/hp-rw-experiment.img
hpmkdir test-dir
# hprm test-dir  # only if appropriate; caution: write is experimental
hpumount
```

The `hpcopy` manual specifically documents copying *from* an HFS+ volume, and `hpmkdir` prints an experimental-write warning. Treat this package as a secondary reader and **limited** writer, not the gold standard for complete metadata support. Use a dedicated HOME/XDG environment in CI to avoid colliding with the command's persistent mount state. [C7]

### 16.7 Other offline readers and visual inspectors

- **HFSExplorer:** Java tool and `unhfs` extractor (GPLv3+), good on Windows/Linux where macOS not available [C4]. Read-only.
- **`libfshfs`:** libyal forensic library/tools and fuzz harnesses, independent format parser with an LGPL-3/GPL-3 license combination [C5]. Refer to its tool READMEs to determine exact binary names/options for the version built.
- **Rust `hfsplus` (0.4.0 as researched):** read-only library for raw images, catalog, xattrs and `decmpfs`; valuable as an additional programmatic oracle but validate its coverage independently [C6].
- **Hex inspection:** `xxd -s 1024 -l 512 image.img`, a small BE field decoder, and `cmp`/`sha256sum` for round-trip byte checks. This **does not** validate catalog consistency, but catches wrong offsets and endian mistakes early.
- **JetBrains `go-dmg-writer`:** a 2026 standalone, cross-platform **write-once** HFS+ builder, directly aligned with fsimg's creation-only goal. It writes UDIF/DMG, supports GPT framing, and documents omission of journaling and resource forks. Apache-2.0: **design and black-box interoperability reference only**, not a BSD-code donor [C9].
- **FreeBSD experimental port:** FreeBSD project's 2025 report [C8] points to a port of Apple's HFS driver with RO/RW mount and basic file/directory operations. This is an **experimental source reference**, not a stable trusted writer; it contains APSL-derived XNU code and must not be copied into BSD-only fsimg.

## 17. Recommended testing matrix and golden-image corpus

### 17.1 Fixtures grouped by features

| Fixture | Source | What it proves | Required independent verifier |
|---|---|---|---|
| Empty HFS+, HFSX | macOS `hdiutil`; Linux `mkfs` | Signature/version, root/thread, empty extents tree, bitmap | `fsck_hfs`, `hfsdump` |
| 512/1024/4096/8192-byte alloc block | `mkfs.hfsplus -b` as supported | Geometry and header overlap | `fsck.hfsplus`, fsimg verifier |
| 4096/8192/32768-byte catalog node | `-n c=...` | Arbitrary node size | `fsck.hfsplus`, hfsfuse |
| One file 0/1/B−1/B/B+1 bytes | macOS mount | Logical vs allocated bytes | Native read; SHA256 |
| Separate resource fork | Native macOS named fork / xattr | Two fork descriptors remain independent | `hfsdump .../rsrc`, macOS `getxattr` |
| 1K/10K sibling files, long names | Native writer or fsimg writer | Multi-leaf, multi-index, records and threads | fsck + independent path listing |
| B-tree node-map overflow | fsimg synthetic / huge catalog | Map-chain capacity >63k nodes (8192 node size) | fsimg validator + read oracle if image feasible |
| At least 9 extents/file fork | fsimg deliberately fragmented | Extents-overflow tree semantics | fsck + content hashes |
| At least 9 extents/resource fork | fsimg synthetic | ForkType `0xFF` and extent keys | hfsdump resource fork |
| Inline and fork-backed large xattrs | macOS-origin image; fsimg | Modern attrs key/data layouts | Native `xattr`, TSK/libyal where supported |
| Unicode/casefold collision set | Apple HFS+, HFSX | Legacy name semantics | Native lookup + raw key ordering |
| File and symlink chains | Apple native | POSIX modes / link fork bytes | Native `readlink` + fsimg |
| Hardlink groups | Apple native | Stub/indirect-node resolution | `stat` inode equivalence + TSK |
| Directory hardlinks | Older macOS/Time Machine fixture | Private/surrogate namespace, cycle guard | TSK/hfsfuse vs fsimg |
| zlib inline/resource `decmpfs` | `ditto --hfsCompression` plus archived fixtures | Logical decompression | Native read hash + hfsfuse |
| LZVN/LZFSE compression | macOS-generated fixtures where codec occurs | Compression variants | Native logical read; explicit type inspection |
| Clean journaled HFS+ | Apple `HFS+J` | JIB/journal detection, empty log | Apple filesystem and fsck |
| Dirty/nonempty journal | Fault-injection VM/crash-created fixture | Safe refusal without replay | `fsck_hfs` on a *copy* |
| HFS-wrapped HFS+ | Older HFS container fixture or `mkfs` variant supporting `-w` | Inner base and length translation | hfsfuse / fsck |
| GPT/APM partitioned volume | macOS `hdiutil -layout GPTSPUD`/SPUD | Shared partition offsets | Native attach and fsimg |
| Image truncated at interesting sizes | Synthetic byte truncation | No OOB access, correct diagnostics | ASan/UBSan fuzz harness |

### 17.2 For every fixture record the oracle evidence

Store `manifest.json` with volume type, originating formatter and version, blockSize, B-tree node sizes, journal state, signature/version, expected file/folder counts, each file's SHA256 of **logical content**, raw data/resource fork lengths/hashes, xattr names/hashes, normalized Unicode paths, POSIX mode, timestamps and hardlink group identity. Include a serializable description of *expected known losses* (e.g. host inability to reproduce FinderInfo), and for generated images save the complete fsimg seed/options.

A single golden image can satisfy multiple fixtures but avoid depending on only one formatter: at minimum collect images from **Apple `hdiutil`** and **Linux `mkfs.hfsplus`**; their differences often expose uninitialized/special fields ignored by overly strict parsers.

### 17.3 Suggested CI gates

1. **Parser unit gates, every commit:** field offset decoders, all record lengths, key comparators, B-tree offset directory, tiny hand-constructed valid/invalid nodes (original test fixtures only).
2. **In-memory property gates, every commit:** valid/invalid structural generators with small fixed resource caps; at least randomized input order to prove sorting/ordering.
3. **Full image creation/extraction, every commit:** small root + nested + max-name + nonempty data/resource + symlink + xattr; run own `verify`, `extract`, and compare manifest.
4. **Linux independent oracle, CI:** `fsck.hfsplus -n`, `hfsdump`, `hfstar`, TSK; optional `hpmount -r` and Rust read-only parser.
5. **macOS native oracle, CI runner or periodic local:** `hdiutil attach` RO, native `fsck_hfs -n`, read and SHA, fresh copy RW with mutation, clean detach.
6. **Large stress/nightly:** 100K/1M objects; 63,488 B-tree-node allocation-bitmap threshold; 255-code-unit names; sparse host source files; fault injection/truncation; all ASan/UBSan checks.
7. **Security/no-regression:** fuzz volume headers, node bounds, xattr/decompression lengths, symlink escapes, private directory NUL names, hardlink cycles, UTF16 invalid sequence handling.

**Exit criterion for Stage 1:** At least two independent implementations can enumerate and read every file of **fsimg-created** HFS+ and HFSX images, Apple native `fsck_hfs -n` (or the strongest available checker) reports a clean filesystem, and **fsimg** can extract known-good images from both major formatters. Writer support means more than passing its own parser.

## 18. Bibliography: format documentation, source code and testing programs

The categories below deliberately distinguish **BSD-license code candidates** from **other-licensed code that may be studied, linked to, or used as an external tool but must not be copied into a BSD-only implementation**. Source file URLs are links, not permission to incorporate source. Individual files may carry a different license from their repository.

### 18.1 Primary published format references

- **[A1] Apple, Technical Note TN1150 — HFS Plus Volume Format** (published March 5, 2004): <https://developer.apple.com/library/archive/technotes/tn/tn1150.html>. Best documented source for on-disk HFS+, HFSX, volume and catalog semantics, B-trees, forks, journaling, hardlinks, Unicode exceptions, wrapper, and consistency requirements. **Documentation source; do not reproduce its copyrightable text or code.** Some details concerning extended attributes were incomplete when published.
- **[A2] Apple open-source HFS header, `core/hfs_format.h`**: <https://github.com/apple-oss-distributions/hfs/blob/main/core/hfs_format.h>. Secondary source to interpret modern catalog flags, attribute records, extra volume bits, Finder info, and special modern fields. **Apple Public Source License 2.0 (APSL), non-BSD; inspect for interoperability facts only.** Companion code: <https://github.com/apple-oss-distributions/hfs>. Debian archived version of the on-disk header: <https://sources.debian.org/src/hfsprogs/332.25-10/include/hfs/hfs_format.h> (also APSL-derived).

### 18.2 BSD-licensed code — permissible candidates subject to individual file review

- **[B1] NetBSD HFS+ `libhfs.c`**, read-side parse/search/extent handling: <https://github.com/NetBSD/src/blob/trunk/sys/fs/hfs/libhfs.c>. The file has an explicit **NetBSD Foundation two-clause BSD-style license**. See also its directory <https://github.com/NetBSD/src/tree/trunk/sys/fs/hfs> and the newer NetBSD source mirror <https://ftp.netbsd.org/pub/NetBSD/NetBSD-current/src/sys/fs/hfs/>. This is the most relevant license-aligned implementation starting point; nevertheless audit each file before reusing it, and don't blindly copy a whole NetBSD subtree.
- **[B2] NetBSD HFS+ declaration and Unicode support**: <https://github.com/NetBSD/src/blob/trunk/sys/fs/hfs/libhfs.h>, <https://github.com/NetBSD/src/blob/trunk/sys/fs/hfs/unicode.c>, and <https://github.com/NetBSD/src/blob/trunk/sys/fs/hfs/unicode.h>. The first two files' license headers were inspected and use the same two-clause BSD-style language; **recheck the header on any additional file**. Related vnode/driver glue: <https://github.com/NetBSD/src/blob/trunk/sys/fs/hfs/hfs_vnops.c>, <https://github.com/NetBSD/src/blob/trunk/sys/fs/hfs/hfs_vfsops.c>. NetBSD licensing overview: <https://www.netbsd.org/about/redistribution.html>.
- **NetBSD case-sensitive lookup fix (2023)**: <https://mail-index.netbsd.org/netbsd-bugs/2023/04/01/msg077752.html>. A very useful concrete interoperability regression: prefix-related HFSX names previously caused wrong-file lookups. Include corresponding `abc`/`abcd` searches as a targeted test, in addition to `A`/`a` case collisions.

### 18.3 Other-license implementations — *links and external oracles only*

- **[C1] hfsfuse**: <https://github.com/0x09/hfsfuse>; <https://github.com/0x09/hfsfuse/blob/master/COPYING>. Read-only FUSE HFS+ driver based on NetBSD, plus `hfsdump` and `hfstar`, with resource forks, hardlinks, xattrs and zlib/LZVN/LZFSE reading. Original portions **MIT** and bundled dependencies under varying terms; review per-file before copying anything; not a BSD-only code source by blanket assumption. Very useful for differential extraction and object inspection.
- **[C2] The Sleuth Kit**: HFS notes <https://github.com/sleuthkit/sleuthkit/wiki/HFS>; source <https://github.com/sleuthkit/sleuthkit>; commands `fsstat`, `fls`, `istat`, `icat`. Forensic independent reader, licenses mixed by source/component; use as an executable oracle or consult behavior, not as copied source without license review.
- **[C3] Linux kernel HFS+ driver**: <https://github.com/torvalds/linux/tree/master/fs/hfsplus>; header <https://github.com/torvalds/linux/blob/master/fs/hfsplus/hfsplus_fs.h>; raw declarations <https://github.com/torvalds/linux/blob/master/fs/hfsplus/hfsplus_raw.h>. Primarily **GPL-2.0**; good for driver behavior, edge cases and Linux RO/RW compatibility; do not copy into BSD-only fsimg.
- **[C4] HFSExplorer**: <https://github.com/unsound/hfsexplorer>. Java read-only extractor and `unhfs`, **GPLv3+**; independent test reader.
- **[C5] libyal `libfshfs`**: <https://github.com/libyal/libfshfs>. Forensic HFS/HFS+ parser and related utilities; **LGPL-3.0 / GPL-3.0 parts**, independent extraction oracle.
- **[C6] Rust `hfsplus` crate**: <https://docs.rs/crate/hfsplus/latest>; published version 0.4.0 as of September 20, 2026. Read-focused parser with HFSX, forks, attrs and compression; check precise source and dependencies; **MIT**, non-BSD license for this user's copy policy.
- **[C7] Unix `hfsplus` userland package**: Debian package manual <https://manpages.debian.org/unstable/hfsplus/hfsplus.7.en.html>, `hpmount` <https://manpages.debian.org/unstable/hfsplus/hpmount.1.en.html>, `hpcopy` <https://manpages.debian.org/unstable/hfsplus/hpcopy.1.en.html>, `hpmkdir` <https://manpages.debian.org/unstable/hfsplus/hpmkdir.1.en.html>, `hpfsck` <https://manpages.debian.org/unstable/hfsplus/hpfsck.1.en.html>. Source index <https://sources.debian.org/src/hfsplus/>. Read-mostly with explicitly experimental write commands; package license must be independently established before any reuse.
- **[C8] FreeBSD experimental HFS+ port**: <https://github.com/stupendoussuperpowers/freebsd_hfs>; FreeBSD status report <https://www.freebsd.org/status/report-2025-04-2025-06/hfs/>. Based in part on Apple/XNU code (APSL); **not** a BSD-licensed source substitute. Potential cross-platform testing target as the project matures.
- **[C9] JetBrains `go-dmg-writer`**, published 2026: <https://github.com/jetbrains/go-dmg-writer>; HFS+ component docs <https://pkg.go.dev/github.com/jetbrains/go-dmg-writer/internal/hfsplus>; overview <https://pkg.go.dev/github.com/jetbrains/go-dmg-writer>. Pure-Go **write-once HFS+** builder and UDIF/GPT container creation; excellent independent architectural reference and test-image generator, but **Apache-2.0 (not BSD), no source reuse under this request's strict policy**. Its documented limitations include absent journaling/resource-fork support and dereferenced hardlinks.

### 18.4 Compression and on-disk special-feature references

- **[D1] HFS+ compression (`decmpfs`) decoder/reference index**: <https://docs.rs/fstool/latest/fstool/fs/hfs_plus/decmpfs/index.html>. Description of compression types and resource-fork decompression, including little-endian headers and chunking; source/license must be independently evaluated before any reuse.
- Also useful: <https://docs.rs/cmpfs/latest/cmpfs/> for compression-codec taxonomy, <https://github.com/0x09/hfsfuse> for a known functional, read-only decompression implementation and fixture comparisons.

### 18.5 Formatters, filesystem checkers and testing documentation

- **[E1] macOS `hdiutil` man page**: <https://keith.github.io/xcode-man-pages/hdiutil.1.html> (`create`, `attach`, `convert`, `-fs HFS+`, `HFS+J`, `HFSX`, `JHFS+X`, `-layout`). Apple-native formatter and mount workflow.
- **[E2] macOS `newfs_hfs` man page**: <https://keith.github.io/xcode-man-pages/newfs_hfs.8.html>. Native low-level new-volume formatter, block size, B-tree node sizing and journal settings.
- **[E3] macOS `fsck_hfs` man page**: <https://keith.github.io/xcode-man-pages/fsck_hfs.8.html>. Reference checker; `-n` means do not repair, `-d` diagnostics.
- **[E4] Linux `mkfs.hfsplus`** (`hfsprogs`): <https://manpages.debian.org/unstable/hfsprogs/mkfs.hfsplus.8.en.html>. Formats HFS+ and HFSX, with block-size, B-tree node, journal options. Apple's old HFS tools source is APSL-derived; treat *binaries* as external testing tools.
- **[E5] Linux `fsck.hfsplus`** (`hfsprogs`): <https://manpages.debian.org/testing/hfsprogs/fsck.hfsplus.8.en.html>. External consistency checker; run `-n` on offline test copies.
- **[E6] Linux kernel `hfsplus` mount**: <https://www.kernel.org/doc/html/latest/filesystems/hfsplus.html> and the GPL Linux source at [C3]; use the local `man mount` for kernel/tool-specific options. Treat journaled RW as unsafe/unsupported by default.
- **[E7] macOS `ditto --hfsCompression`**: <https://keith.github.io/xcode-man-pages/ditto.1.html>. Possible reference fixture generator for native `decmpfs` (always inspect resulting physical compression type).

### 18.6 Additional browsing pointers

- <https://github.com/apple-oss-distributions/hfs> — Apple formatter, checker and filesystem sources (APSL).
- <https://github.com/Artoria2e5/hfsprogs> — Linux/Darwin HFS+ utilities mirror; verify license and exact revision.
- <https://www.sleuthkit.org/sleuthkit/man/> — Sleuth Kit user-facing tool documentation.
- <https://www4.netbsd.org/about/redistribution.html> — NetBSD redistribution statement.

## 19. Implementation checklist for fsimg

Use these as incremental milestones and acceptance criteria. They assume the existing fsimg library owns generic image I/O, archives/extraction, path and metadata interfaces and allocation-backed output sinks.

### 19.1 Writer: minimum complete filesystem

- [ ] Parse input tree without mutating it; preserve raw file/resource-fork and selected xattr streams in an intermediate representation.
- [ ] Choose target volume bytes, allocation size, signature `H+`/`HX`, name comparator, B-tree node sizes and deterministic-date policy.
- [ ] Encode HFS-specific UTF-16 decomposed names, reject reserved/illegal or compare-equivalent collisions, handle host colon/slash names.
- [ ] Allocate unique CNIDs >=16 and generate **both** primary and thread records per file/folder, plus root special records.
- [ ] Build a generic sorted B-tree packer (node descriptors, index keys, leaf siblings, record offsets, node maps, empty trees).
- [ ] Build allocation and overflow metadata files, even for an otherwise empty image; keep the extents tree itself within <=8 extents.
- [ ] Store both file forks and reference them with correct ordered extents; initialize empty forks correctly.
- [ ] Encode volume header and alternate, reserve boot/trailing sectors, compute bitmap + `freeBlocks`, set volume counts/flags.
- [ ] Run the internal independent verifier on the generated image before publishing the output filename.
- [ ] Pass external `fsck_hfs`/`fsck.hfsplus`; native and hfsfuse extraction match logical file checksums.

### 19.2 Reader: extraction that is safe for untrusted images

- [ ] Probe `H+`, `HX`, raw-offset, selected partition or HFS wrapper, enforcing bounded I/O.
- [ ] Decode headers/forks without native-layout assumptions and reject impossible lengths before allocating buffers.
- [ ] Read and search the catalog B-tree with proper HFS+ / HFSX comparator; enumerate in leaf order and verify thread backreferences.
- [ ] Map data/resource fork bytes through inline extents and extents-overflow keys; stream rather than buffer entire forks.
- [ ] Expose raw Finder bytes, UTC and creation timestamps, permission fields, symlinks, special file types.
- [ ] Extract normalized names through a collision-safe, sanitizing host path adapter and prevent symlink traversal escapes.
- [ ] Read inline and fork-backed attributes; avoid duplication of synthetic `com.apple.FinderInfo`/`com.apple.ResourceFork`.
- [ ] Recognize journal state, refuse potentially stale base image in strict mode, allow recovery through an explicit flag.
- [ ] Reconstruct regular hardlinks and optionally directory-hardlink semantics.
- [ ] Decode `decmpfs` as logical contents while preserving accessible raw fork/xattr representations.

### 19.3 Security and compatibility gates

- [ ] Every on-disk integer is big-endian **except specific embedded subformats**, notably `decmpfs` metadata.
- [ ] Byte-to-block arithmetic is checked in 64-bit/128-bit intermediates (or explicit checked operations).
- [ ] Never trust B-tree key lengths, footer offsets, node count, journal sizes, `dataSize`, or decompressed size without caps.
- [ ] Fuzz corrupt cyclic sibling links, block ownership collisions, reserved-volume overlap, excess extents, overflow trees, dangling CNIDs, overlapping catalog keys and fork overflows.
- [ ] Run cross-implementation differential tests on files and directories with case-sensitive prefix collisions (`abc`, `abcd`), not only case-only collisions (`abc`, `ABC`).
- [ ] Preserve unsupported raw metadata in manifests or report loss; never silently fabricate special ACL, Finder or hardlink semantics.
- [ ] Maintain a **license provenance list per imported source file**; restrict actual reused code and test fixtures to in-house or individually cleared BSD-license material.

## 20. Known uncertainties and deliberately deferred cases

These are places where TN1150 alone should **not** be considered a sufficient contemporary write specification. They are explicit research/test tasks, not implicit assertions that fsimg implements them.

| Area | Gap / why to test | Safe current policy |
|---|---|---|
| HFSX Unicode edge behavior | Historical Apple's casefold version and variant-specific normalization vary by age | Read raw and normalized paths; freeze proven comparison table before writer release |
| Modern expanded dates | Newer volume/catalog flags exist but epoch/encodings vary | Preserve raw time fields; don't set expanded flags on creation |
| Attribute-tree order | Current implementations and old TN1150 text differ on name key, comparison, and inline format | Prefer modern key/records in §8; validate with Apple-written fixture |
| Finder and resource fork virtual attrs | Native xattr APIs synthesize entries from catalog fields | Preserve raw 32 bytes and fork as separate physical objects |
| Large attrs and extent-overflow | Nontrivial B-tree key ranges and fork layout | Read carefully; writer initial scope inline only |
| Directory hardlinks / Time Machine | Private directory relocation and synthetic namespace are complex | Read by CNID with cycle guard; do not generate until independently tested |
| `decmpfs` resource codecs | Several chunk/resource-framing variants beyond simple deflate | Bounded decoder per type; reject unknown codec without pretending to extract |
| Nonempty journal | Correct journal checksum/replay and sector overlay required | Strictly refuse or explicitly extract potentially stale state |
| Original HFS wrapper | Creating a real outer HFS filesystem requires more than an MDB | Reader-only optional; don't emit fake wrapper |
| Very large trees / hot files | Map-node chains and undocumented hot-file indexes appear on large active volumes | Parse standard B-trees; ignore nonessential hotfile hints; stress writer |
| Bootable Apple system volumes | Blessing, startup file, Finder boot metadata, OS-specific signatures and OS expectations | Target **data-volume interoperability**, not bootability |

**Final distinction:** This specification aims to make `fsimg` a correct **reader and fresh-volume constructor**. It does *not* specify a complete filesystem implementation, a crash-safe updater, a journal replayer, boot-volume builder or a transparent host filesystem. That narrower contract makes it feasible to ship a reliable HFS+ writer without years of filesystem-driver work.
