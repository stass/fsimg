# EROFS filesystem: on-disk specification and `fsimg` implementation plan

**Edition:** research / implementation specification, 6 November 2026  
**Project:** [`stass/fsimg`](https://github.com/stass/fsimg), `master` (BSD-2-Clause, C11)  
**Target:** detect, inspect, list, read, extract, fully verify and create EROFS images without mounting or privileges on Linux, FreeBSD and macOS  
**Status:** architecture- and source-researched; not a declaration that the proposed driver or test cases have been implemented or executed  
**Current interoperability baseline:** `erofs-utils` 1.9.4 (19 August 2026); its `dev` source may contain later work.

> **Independent-authoring and provenance restriction.** This is original explanatory and engineering prose describing publicly documented interoperability facts, measurements, record offsets, field interpretations and proposed algorithms. It contains **no copied kernel, GPL, proprietary or other third-party implementation source**. The Linux kernel and the utilities are *external reference implementations / test oracles*, not permission to import code. `erofs-utils` changed its licensing in July 2026: its current `COPYING` says most `lib/` and `include/` code is **GPL-2.0+ OR MIT** and other code is usually MIT, subject to each file's own SPDX tag. Those MIT grants can be compatible with BSD-2-Clause when followed correctly, but this document does **not** rely on code transplantation. For any prospective imported file, inspect the exact revision, full SPDX/provenance, transitive dependencies and notices separately. The Linux kernel `fs/erofs/` is generally GPL-2.0, and must not be pasted into `fsimg`. Use externally-generated images as fixtures subject to source/redistribution review.

**Evidence classes** used throughout:

- **N** — published EROFS on-disk documentation, suitable as format requirements.
- **H** — permissively licensed, currently maintained `erofs-utils` on-disk header; concrete offsets and constants (interoperability facts).
- **I** — behavior inferred from cross-checking `erofs-utils` user-space and Linux kernel implementations; validate against images before declaring a writer profile complete.
- **E** — newer/experimental, evolving, or implementation-dependent extension; qualify by upstream version and independent fixtures.
- **P** — proposed `fsimg` product behavior/architecture, **not** part of the EROFS wire specification.

This document distinguishes **format-valid image**, **`fsimg` supported reader profile**, **`fsimg` writer profile**, and **verified interoperability**. Features not yet supported must be reported as unsupported, never treated as damaged or silently interpreted as something else.

---

## 0. Executive design decision [P]

EROFS is a particularly strong fit for `fsimg`: it is fundamentally a read-only, compact image format, not a mounted read/write volume. Use **one public format name, `erofs`**, with one descriptor and explicitly reported feature/profile details. There is no persistent allocation bitmap, mutable journal, fixed inode table or arbitrary filesystem repair flow to implement.

**Recommended delivery sequence:**

1. **Reader A: core, uncompressed.** Superblock, CRC32C, compact/extended inodes, flat/plain and flat/inline file payload, directories, symlinks, owner/mode/mtime, hard links, full validation, zero-range semantics. Include unknown optional feature handling, strict volume slicing and multi-platform operation.
2. **Reader B: xattrs and chunks.** Inline/shared attributes, ACL/SELinux metadata loss reporting, optional long prefixes and Bloom filters; local chunk maps, holes, dedupe and external-device references when a safe way to supply those devices exists. Defer external-device reading in the public API if it requires an API extension; do not mistake a missing blob for zeros.
3. **Reader C: compression.** EROFS compressed indexes (full and compact), LZ4 and MicroLZMA first, then DEFLATE and Zstd; big pclusters, alternative algorithms, compressed tails, packed fragments and deduplication. Compressed directory data must work via the exact same content resolver.
4. **Writer A: default interoperable raw image.** Deterministic, sparse-aware, standalone, 4KiB-block image with optional inode tail packing. Plan all inode NIDs, directory records, metadata offsets and data placement in bounded scratch-backed passes. Implement the simplest legal layout before adding compression.
5. **Writer B: compact modern image.** xattrs, chunk dedupe, LZ4 compression, compressed tails/fragments, then further algorithms and external data files. Qualify each combination with `fsck.erofs`, `dump.erofs`, `erofsfuse`, and Linux kernel mounts, plus a separately authored byte-level checker.
6. **Later/experimental:** `48BIT` addressing and byte-oriented encoded extents, directory compression, metadata zone/metabox compression, image-share xattrs, OCI/tar-index integrations, S3 and incrementally constructed images. The initial writer should **not** enable experimental bits by default.

A reader that supports compressed extents, but not the codec used in a particular image, must be able to open/report the image and produce `FSIMG_EUNSUPPORTED` upon a request requiring that codec; whether opening can succeed depends on whether the root directory itself uses the unsupported representation. Full verification cannot claim completeness in that case.

### 0.1 Image concepts [N/H]

| Term | Meaning |
|---|---|
| Filesystem block `B` | `1 << blkszbits`, commonly 4096 bytes. Blocks normally address file payloads; metadata can be byte-granular. |
| NID | Inode slot number: ordinarily `meta_blkaddr * B + NID * 32` is the byte address of its inode. It is **not** a directory-entry index or the inode's `i_ino`. |
| Inode body | One 32-byte compact or 64-byte extended inode plus any per-inode extended attributes, indexes and/or inline tail. |
| Data layout | Per-inode choice: uncompressed flat, compressed full, uncompressed with inline tail, compressed compact, or chunked. |
| Logical cluster | Compression indexing granule, potentially distinct from filesystem block size. |
| Physical cluster (pcluster) | One stored compressed (or raw) extent representing some logical bytes, potentially spanning multiple blocks. |
| Packed inode | Special file holding fragments/tails used by other files. |
| Metabox inode | Special file holding *metadata address space* bytes, optionally compressed; new experimental extension. |
| Device table | 128-byte slots on the primary device mapping secondary data blobs and/or unified block ranges. |
| Volume | A slice of the container/disk handed to `fsimg`; **all offsets in this specification are relative to that slice** unless stated otherwise. |

### 0.2 fsimg contracts specifically observed in the repository [P]

The repository's [`lib/formats/format.h`](https://github.com/stass/fsimg/blob/master/lib/formats/format.h) and [`format.c`](https://github.com/stass/fsimg/blob/master/lib/formats/format.c) require a **pure, prefix-only, length-bounded** `probe`, and independently optional `reader`/`writer` hooks. Register a new `fsimg_erofs_format` in the static `formats[]` array; no common-library switch on format names should be needed. Use the existing [`lib/read/reader.h`](https://github.com/stass/fsimg/blob/master/lib/read/reader.h) callbacks `open`, `prepare`, `replay`, `keys`, `key`, `resolve`, `verify`; an EROFS `resolve` must yield a contiguous mapped, zero, or bounded held-data extent covering the requested byte. Writers use [`lib/write/writer.h`](https://github.com/stass/fsimg/blob/master/lib/write/writer.h)'s `validate`, `plan`, `emit`, `free`. Prior analogous layouts are in [`lib/formats/iso9660/`](https://github.com/stass/fsimg/tree/master/lib/formats/iso9660) and [`lib/formats/hfsplus/`](https://github.com/stass/fsimg/tree/master/lib/formats/hfsplus).

`fsimg` currently bounds memory via a context budget (16MiB minimum), supports scratch-backed metadata, `--mtree`, `-R` reproducible images, `-M` minimum sizing, `cW` verify-before-publish, `Wdf` full verify and nested container layers. EROFS must preserve these properties. The common metadata model **does not yet retain raw xattr values in exported entries**: declare `FSIMG_META_EXTRA_DATA` for xattr-bearing entries, avoid claiming an exact xattr round-trip, and consider a deliberate common API extension to expose xattr values later. The current source catalog rejects device nodes and sockets for creation (reader-only kinds); EROFS writer advertisements must reflect that existing constraint instead of claiming every on-disk POSIX kind is writable.

## 1. Identification, endian, geometry and volume bounds [N/H/P]

All EROFS multibyte on-disk scalar fields in the documented profiles are **little-endian**, regardless of host CPU. Decode integer bytes explicitly after bounds checks; do not overlay a compiler structure on untrusted mapped bytes or use unaligned casts.

**Canonical filesystem identity:** an unsigned 32-bit little-endian value `0xE0F5E1E2` at byte offset **1024** from the start of the EROFS volume, little-endian byte sequence **`E2 E1 F5 E0`**. The usual first 1024 bytes are reserved for boot/other use. Minimum valid readable slice must cover superblock bytes used (at least 1152 bytes for base 128B). EROFS may be embedded as a partition, inside a compressed/container layer, or at an explicit caller-chosen offset; do not scan arbitrary offsets or apply container-relative offsets twice.

**Probe** (`probe_bytes` at least 1152, optionally 1168):

1. If fewer than 1028 bytes are available, return `FSIMG_EFORMAT` — signature cannot be seen. If byte 1024 magic differs, return `FSIMG_EFORMAT`, without claiming anything about the prefix's boot code.
2. Once magic matches, *claim* EROFS. Check that full base header exists; `blkszbits` is legal for the chosen reader profile (9–12 in current upstream Linux/erofs-utils), `dirblkbits` is supported (0 for current profiles), `128 + 16*sb_extslots` stays within the accessible superblock region, and features are understood. Invalid header geometry: `FSIMG_ECORRUPT`; well-formed unsupported block size or incompatible feature: `FSIMG_EUNSUPPORTED`. Probe must not fetch secondary metadata, allocate, or read beyond its declared prefix.
3. Open repeats all validation, expands to real volume bounds, checks CRC when enabled, then accesses the root inode with bounded I/O. `FSIMG_READ_ASSUME_IDENTITY` only waives magic identity, not geometry or safety checks.

**Important ambiguity:** EROFS does *not* reserve the entire first 32KiB for its own system area as ISO9660 does. Do **not** set `reserved_system_area=true` just because the superblock starts at 1024. Existing MBR/GPT detection should treat EROFS according to fsimg's layer-vs-filesystem claim/selection rules. A boot sector may coexist at bytes 0–1023, but that alone does not mean a partition table is a second independent interpretation of this same volume.

Sizes: `B=2^blkszbits` (512, 1024, 2048, 4096 supported by current common implementations; exact platform restrictions can differ). An advertised `blocks_lo` or `inos` is primarily accounting information and may legally be zero; **never trust it to validate addresses**. The actual `fsimg` slice length and independently supplied external-device lengths are the authority for every read. File size for extended inodes is `u64`; fsimg must check `offset+length` overflow on each operation. `volume_name` is 16 raw bytes, not guaranteed NUL terminated; expose a bounded decoding/presentation policy, and on write limit to <=15 bytes for compatibility with `mkfs.erofs -L` rather than assume arbitrary UTF-8 fits.

## 2. Superblock layout [N/H]

All offsets in this table are **relative to the superblock start at volume offset 1024**. `LE` indicates unsigned little-endian except `epoch`, whose time interpretation is signed; `raw` is uninterpreted bytes. The 128-byte base and optional 16-byte extension slots end at `0x80 + 16*sb_extslots`.

| Offset | Bytes | Field | Interpretation / caveat |
|---:|---:|---|---|
| `0x00` | 4 | `magic` | `0xE0F5E1E2` |
| `0x04` | 4 | `checksum` | CRC32C when compat bit 0 set |
| `0x08` | 4 | `feature_compat` | Compatible capability bitmap |
| `0x0C` | 1 | `blkszbits` | `B = 1U << bits`, >= 9 |
| `0x0D` | 1 | `sb_extslots` | Superblock length `128+16*n` |
| `0x0E` | 2 | `rootnid_2b` / `blocks_hi` | Old root NID, or high bits of block count in 48-bit variant |
| `0x10` | 8 | `inos` | Advisory count of files/inodes |
| `0x18` | 8 | `epoch` | Base UNIX seconds (compact timestamp basis) |
| `0x20` | 4 | `fixed_nsec` | Compact-inode shared nanoseconds, must be <1e9 |
| `0x24` | 4 | `blocks_lo` | Low 32 bits of primary device block count |
| `0x28` | 4 | `meta_blkaddr` | Start of inode metadata address space, in blocks |
| `0x2C` | 4 | `xattr_blkaddr` | Shared xattr pool's block address when direct |
| `0x30` | 16 | `uuid` | 128-bit filesystem identifier |
| `0x40` | 16 | `volume_name` | Raw label bytes; may omit terminating NUL |
| `0x50` | 4 | `feature_incompat` | Must-understand format extensions |
| `0x54` | 2 | `available_compr_algs` / `lz4_max_distance` | Compression feature changes interpretation; in core zero |
| `0x56` | 2 | `extra_devices` | Number of secondary blob devices |
| `0x58` | 2 | `devt_slotoff` | 128-byte record index of device table |
| `0x5A` | 1 | `dirblkbits` | Directory block size `B<<dirblkbits` (currently 0) |
| `0x5B` | 1 | `xattr_prefix_count` | Number of long shared attribute name prefixes |
| `0x5C` | 4 | `xattr_prefix_start` | Offset/pointer to prefix table (placement depends on flags) |
| `0x60` | 8 | `packed_nid` | Special packed-fragment inode NID, when used |
| `0x68` | 1 | `xattr_filter_reserved` | Filter inapplicable if nonzero |
| `0x69` | 1 | `ishare_xattr_prefix_id` | Prefix-table index for content fingerprint convention |
| `0x6A` | 2 | reserved | Reject unsupported nonzero semantics in strict writer; reader preserve/report |
| `0x6C` | 4 | `build_time` | Build seconds relative to `epoch` in newer layout |
| `0x70` | 8 | `rootnid_8b` | 48-bit-layout root NID if in use |
| `0x78` | 8 | reserved2 | Reserved |
| `0x80` | 8 | `metabox_nid` | Requires METABOX and a superblock extension covering this field |
| `0x88` | 8 | reserved3 | Reserved (when extension slot covers it) |

**Superblock CRC32C:** if `feature_compat & 1`, compute Castagnoli CRC32C over the superblock-bearing tail of the filesystem block, with checksum bytes treated as zero. **Known specification/reference discrepancy:** the published core guide describes bytes `[1024, 1024+B)`, but the current Linux kernel and `erofs-utils` implementations measure **`B-1024` bytes starting at offset 1024 when `B>1024`, and `B` bytes starting at 1024 when `B<=1024`**. For a 4096-byte block, that is `[1024,4096)`, not `[1024,5120)`; for a 512-byte block it is `[1024,1536)`. The implementation and independent fixtures should decide interoperability. The upstream implementation uses CRC32C seeded with `0xFFFFFFFF` (non-final-inverted form; test vector validation required). **Do not** use generic zlib CRC32 (IEEE), and do not accidentally include the reserved first 1024 bytes in the digest. This discrepancy needs a documented test against a 512B, 1KiB and 4KiB `mkfs.erofs` image before finalizing implementation.

**Superblock extension:** guard the extension count before multiplying. The well-defined `metabox_nid` starts at superblock relative `0x80`; that field is absent if `sb_extslots == 0`. The superblock/extension must fit the on-disk block layout accepted by the reader; don't reinterpret uninitialized extension bytes as fields. `sb_extslots` is *not* a compression algorithm count. Compression parameter records are stored in a separate superblock-associated configuration area (see Section 8).

## 3. Feature negotiation and compatibility [N/H/E/P]

**Core rule:** unknown `feature_incompat` bits must refuse content decoding (`FSIMG_EUNSUPPORTED`); recognized combinations with unimplemented interpretation likewise refuse operations that depend on them. Unknown compatible bits can be ignored *only* if they do not invalidate semantics that `fsimg` claims to verify. Preserve unknown bits in information output.

### 3.1 `feature_compat`

| Mask | Meaning | Reader treatment |
|---:|---|---|
| `0x00000001` | `SB_CHKSUM` | CRC32C required for full verification and opening as policy; bad CRC is corrupt, not unsupported |
| `0x00000002` | `MTIME` | Extended timestamp is mtime rather than earlier possible ctime; report correctly |
| `0x00000004` | `XATTR_FILTER` | Optional inverted 32-bit xattr name Bloom filter; correctness must never rely on false negatives |
| `0x00000008` | `SHARED_EA_IN_METABOX` | Shared xattrs come from decoded metabox data |
| `0x00000010` | `PLAIN_XATTR_PFX` | Prefix table in standalone region rather than special packed/metabox inode |
| `0x00000020` | `ISHARE_XATTRS` | Shared content fingerprints in selected xattr namespace/prefix |

### 3.2 `feature_incompat`

| Mask | Alias/name used in upstream format | Consequence |
|---:|---|---|
| `0x00000001` | `LZ4_0PADDING` | Special compression input framing/padding semantics |
| `0x00000002` | `COMPR_CFGS` / legacy `BIG_PCLUSTER` alias | Compression configuration area and larger pclusters |
| `0x00000004` | `CHUNKED_FILE` | Chunk-based inode datalayout 4 supported |
| `0x00000008` | `DEVICE_TABLE` / `COMPR_HEAD2` alias | Extra devices / secondary compression head in applicable contexts |
| `0x00000010` | `ZTAILPACKING` | Encoded tail may live with inode metadata |
| `0x00000020` | `FRAGMENTS` / `DEDUPE` alias | Shared packed compressed tail/file fragments; deduped extents |
| `0x00000040` | `XATTR_PREFIXES` | Long xattr name-prefix table |
| `0x00000080` | `48BIT` | **Experimental** extended addresses / encoded byte-oriented extents, Linux 6.15+ |
| `0x00000100` | `METABOX` | **Experimental** metadata-as-file/compressed metadata, Linux 6.17+ |

**Alias warning:** the repeated numeric bits in upstream definitions are **not** distinct feature bits and cannot be checked as such. The meanings are coupled by on-disk version/history and data-layout context. Do not create independent enum values that accidentally double-count the same bit; save raw flags and decode the whole feature combination.

The format may carry both compatible and incompatible extensions; each image's exact flags must accompany each test fixture. Adding support for a bit implies supporting its required cross-feature invariants, not only accepting the flag.

## 4. Inode addressing, fields and semantics [N/H]

### 4.1 Locating an inode

For conventional metadata, `inode_byte_offset = (u64)meta_blkaddr*B + (u64)nid*32` relative to the primary EROFS volume. `nid` identifies the **start of the inode** in the 32-byte slot space, not a count of consecutive records. Extended inodes consume two slots; adjacent inode bodies may be packed tightly, and variable metadata follows. Validate the entire extent of every record and its attached metadata before reading it; do not assume the inode is contained in one block. Under METABOX, high NID bit 63 selects an entirely different logical metadata address space delivered via the metabox inode; see Section 11.

The `i_ino` field at relative offset `0x14` is a stat-compatible serial number, **not** the on-disk addressing key. Hardlinks are represented by multiple dirents with the **same NID**, not by copying the inode or trusting equal `i_ino` numbers. Directory cycles and hardlinks require per-walk visit protection.

### 4.2 `i_format`

`i_format` is LE16 at inode offset 0. Bits are: bit 0 inode version (0 compact 32B, 1 extended 64B); bits 1–3 are `datalayout = (i_format >> 1)&7`; bit 4 has 48-bit-layout-specific meaning (directory dot omission, compact non-directory `nlink==1` enabling high block bits); bits 5–15 reserved. Layout IDs:

| ID | Layout | Basic interpretation |
|---:|---|---|
| 0 | `FLAT_PLAIN` | One consecutive run of uncompressed blocks |
| 1 | `COMPRESSED_FULL` | Compressed map with 8-byte full indexes or newer variable-size extent records |
| 2 | `FLAT_INLINE` | Leading full blocks plus trailing inline bytes in inode metadata block |
| 3 | `COMPRESSED_COMPACT` | Compression map encoded in dense 2/4-byte index packs |
| 4 | `CHUNK_BASED` | Fixed-size chunks and 4B/8B address indexes |
| 5–7 | reserved | Unsupported (do not guess) |

### 4.3 Compact 32-byte inode

| Offset | Bytes | Field / interpretation |
|---:|---:|---|
| `0x00` | 2 | `i_format` |
| `0x02` | 2 | `i_xattr_icount`; `0` none; otherwise xattr-region size `12+4*(count-1)` |
| `0x04` | 2 | POSIX `i_mode` including file-type bits |
| `0x06` | 2 | `i_nlink`, or 48-bit-format high block/cluster address bits when bit4 indicates implicit nlink=1 |
| `0x08` | 4 | `i_size`, LE32 |
| `0x0C` | 4 | **Core:** reserved, must be zero. **48BIT extension:** LE32 `i_mtime` seconds delta relative to superblock `epoch` |
| `0x10` | 4 | Union: `startblk_lo`, compressed-blocks count, `rdev`, or `[u16 chunk_format,u16 reserved]` |
| `0x14` | 4 | `i_ino`, stat-compatible serial |
| `0x18` | 2 | UID |
| `0x1A` | 2 | GID |
| `0x1C` | 4 | Reserved |

**Normative core:** without `48BIT`, offset `0x0C` is reserved and should be zero, and all compact inode timestamps are `(epoch, fixed_nsec)`. **48BIT extension:** interpret that LE32 word as a per-inode seconds delta; timestamp becomes `(epoch + delta, fixed_nsec)` with checked arithmetic. **Upstream-implementation nuance:** the current `erofs-utils/lib/namei.c` decodes `epoch + i_mtime` even without checking `48BIT`; this yields the same result for conforming core images (zero reserved word) but can differ on malformed core images with nonzero reserved bytes. A strict `fsimg` full verifier should reject such nonzero reserved bytes; its reader compatibility policy may choose to match the permissive upstream behavior but must not treat the malformed image as conforming. Compact UID/GID cannot preserve arbitrary 32-bit IDs; writer should select an extended inode when needed.

### 4.4 Extended 64-byte inode

| Offset | Bytes | Field / interpretation |
|---:|---:|---|
| `0x00` | 2 | `i_format` |
| `0x02` | 2 | `i_xattr_icount` |
| `0x04` | 2 | `i_mode` |
| `0x06` | 2 | Reserved, or high address component in 48BIT variants when applicable |
| `0x08` | 8 | LE64 file size |
| `0x10` | 4 | `i_u` union, as for compact |
| `0x14` | 4 | `i_ino` |
| `0x18` | 4 | UID |
| `0x1C` | 4 | GID |
| `0x20` | 8 | Unix time seconds (as signed timestamp) |
| `0x28` | 4 | Nanosecond field, range 0..999,999,999 |
| `0x2C` | 4 | Hard-link count |
| `0x30` | 16 | Reserved |

On non-48BIT images, reserved high address bits are not a license to read beyond 32-bit block numbers. On special file types, `i_u` carries an encoded device number (for character/block devices) or is not meaningful; `i_size` is not always meaningful for those types. Preserve unknown file-type claims as unsupported, not regular files.

### 4.5 Metadata following inode [N/H]

For ordinary inodes, physical order is: **fixed 32/64-byte inode**, then **inline xattr region** (possibly empty), then any layout-specific compact/full compressed map or flat inline payload. A flat inline payload begins at `inode_offset + fixed_inode_size + xattr_region_bytes`. For compressed indexes, consult their separate 8-byte alignments; for chunk indexes, align to entry size. A long xattr region or an extended inode can force a fallback from FLAT_INLINE to FLAT_PLAIN when tail data would cross a block boundary.

### 4.6 File kinds, ownership and timestamp propagation [N/P]

Interpret mode with POSIX `S_IFMT`: regular, directory, symlink, FIFO, socket, character, block. A symlink target is *file content bytes* and may be inline, plain, chunked, or compressed, not a separate zero-terminated path field. The current `fsimg` symlink model accepts targets 1–1024 bytes and disallows embedded NUL; report unsupported for a target outside that supported profile. Hardlinks must not be reported as independently unrelated files in a format that intends to preserve the link relation; verify consistent inode metadata across repeated NIDs. For device nodes, decode EROFS/Linux device encoding with careful major/minor range checks, but `fsimg` extraction never creates device nodes and writer source admission currently excludes them.

`MTIME` matters to the interpretation of extended `i_mtime`: early EROFS images can have ctime semantics. Do not misleadingly label every legacy timestamp an mtime. `fsimg` currently carries both modified and changed metadata; map to the correct one according to feature flag. Core compact inodes share both `epoch` and `fixed_nsec`; under `48BIT`, the reserved seconds word becomes an individual LE32 delta from `epoch`. This distinction affects writer choice of compact versus extended inodes. File atime and birthtime are **not** native EROFS inode fields.

## 5. Flat file data and holes [N/H/P]

For file size `S`, `B` block bytes, and flat `startblk` from `i_u` (plus permitted 48-bit high component):

**FLAT_PLAIN (layout 0):** bytes `[0,S)` are stored contiguously from volume byte offset `startblk * B`. The allocation covers `ceil(S/B)` blocks. For zero-size files there is no required payload allocation. All-one address is used as an EROFS null/unmapped address for sparse flat files in newer implementations; treat as known zeros only where the relevant format rules define it. Do not interpret an arbitrary out-of-range block address as a hole.

**FLAT_INLINE (layout 2):** full leading blocks `floor(S/B)` are at `startblk*B`; remaining `R=S%B` bytes are at `inode_offset+inode_bytes+xattr_region_bytes` in the inode's metadata block. A file smaller than one block may be *entirely* inline. An exact block-multiple size has no tail, and the writer should normally select FLAT_PLAIN rather than emit meaningless inline records. The exact inline region must fit in a **single filesystem block**; reject crossing metadata block boundaries. The rounded-up allocation of an inline short file does **not** imply bytes after EOF are readable content.

**Zero extents:** `fsimg` uses a first-class known-zero-range model; use it for holes in chunked or sparse flat files. An unwritten output hole is a physical storage optimization, not by itself proof of the file's intended logical zero bytes. Build from canonical source range information, verify every tail, do not scan a massive image's unallocated space.

## 6. Directory encoding and name rules [N/H]

Each directory is an inode whose bytes, delivered through the same uncompressed/compressed file resolver, form a sequence of directory blocks. Current `dirblkbits=0`: `directory_block_size=B`. Every nonempty directory block starts with an array of 12-byte fixed-size entries and is followed by a tightly concatenated name region. Entry offsets relative to the directory-block start:

| Entry offset | Bytes | Description |
|---:|---:|---|
| `0x00` | 8 | LE64 child NID; high bit may select metabox namespace |
| `0x08` | 2 | LE16 byte `nameoff` in current directory block |
| `0x0A` | 1 | `file_type` enum |
| `0x0B` | 1 | Reserved byte |

`entry_count = first_entry.nameoff / 12`. Require count at least 1 on nonempty block, `first_nameoff` within the *actual valid directory-block contents*, a multiple of 12, and every record complete. Names start at each `nameoff`; all except last have length `next.nameoff - this.nameoff`, while the last is bounded by a zero terminator or by the valid end of directory block according to the upstream padding convention. Names must contain neither `/` nor embedded NUL; length 1–255 bytes; there is no mandated Unicode encoding (UTF-8 recommended). Directory records (including `.` and `..`) are lexicographically sorted by **raw filename bytes**, strictly increasing for ordinary names. Do not casefold, normalize UTF-8, or sort by locale on write. Filesystem lookups are byte-exact and case-sensitive.

File types: `0` unknown, `1` regular, `2` directory, `3` character device, `4` block device, `5` FIFO, `6` socket, `7` symlink. If `file_type=0`, the target inode's `i_mode` is authoritative; otherwise verify consistency on full walk. Reject reserved type values unless a future specification defines them.

**Dot rules:** classic directories contain `.` pointing to their own NID and `..` to parent (root parent is itself). New 48BIT `DOT_OMITTED` may remove `.` from the on-disk directory and require it to be synthesized logically; do not incorrectly reject these directories if the extension is supported. Validate `..` parent references and avoid recursively following either special entry.

**Directory verification:** make a bounded scan per physical/logical directory block, check bytewise key ordering, record offsets, NID ranges (against actual addressable data, *not* advisory `inos`), all referenced inode types, path collisions, exactly one correct `..` (subject to supported variant), and directory-graph acyclicity. Do not require physical record ordering across directory blocks beyond the ordering rules established by the current format; preserve reliable lookup semantics. Empty-directory inode layout still needs root/parent semantic tests.

For `fsimg` directory callbacks, `prepare` must decode one directory and admit children through common `fsimg_directory_admit`, with only a bounded block and scratch-backed path/child structures; `replay` rereads or validates the selected record and fills the common entry. Use a **portable byte-exact name profile** that does not silently normalize invalid UTF-8. Existing fsimg name-validation and unsafe-output-name policies must be applied; EROFS's 255-byte limit does not guarantee every byte sequence is a safe host path.

## 7. Extended attributes (xattrs), ACLs and shared pools [N/H/P]

### 7.1 Per-inode xattr body

If `i_xattr_icount==0`, no xattr body exists. Otherwise exactly `12+4*(i_xattr_icount-1)` bytes follow the fixed inode. The first 12 bytes are:

| Offset | Bytes | Meaning |
|---:|---:|---|
| `0` | 4 | LE32 `h_name_filter`: inverted 32-bucket name filter when enabled |
| `4` | 1 | `h_shared_count`: number of referenced shared entries |
| `5` | 7 | Reserved bytes |

After those 12 bytes come `h_shared_count` LE32 shared-xattr indices, followed by zero or more inline xattr entries, each aligned to 4 bytes. Every index requires four bytes and must fit the declared body; a body with nonzero entry count cannot be shorter than its 12-byte header.

Individual xattr record:

| Relative offset | Size | Meaning |
|---:|---:|---|
| `0` | 1 | Name suffix length, `e_name_len` |
| `1` | 1 | Namespace selector, `e_name_index` |
| `2` | 2 | LE16 value length |
| `4` | variable | Name suffix bytes, immediately followed by value bytes; padded to next 4B boundary |

Recognized short namespaces: `1=user.`, `2=system.posix_acl_access`, `3=system.posix_acl_default`, `4=trusted.`, `6=security.`. These are *prefixes*; the ACL indexes represent complete names and usually have zero suffix. Some header versions reserve `5` for `LUSTRE` but the public current generic xattr document does not define its string: **treat unrecognized namespace values as unsupported metadata, not as guessed names**. `e_name_index` high bit `0x80` means the low seven bits index the long-name prefix table; reconstruct `builtin_prefix + infix + suffix`.

### 7.2 Shared xattr entries

The default shared area begins at byte offset `xattr_blkaddr*B`; one shared index means **`index*4` byte offset** from the start of that area. Each shared entry uses the same entry encoding and alignment as inline entries. When `feature_compat` bit `0x8` is enabled, the shared area is instead within the metabox inode's *decoded data*, not at a physical block. Verify each referenced entry, sharing legality and deduplicated name consistency independently; retain a cache with byte-budgeted eviction, not an in-memory table proportional to all shared entries.

### 7.3 Long prefixes and placement

When `feature_incompat & 0x40`, `xattr_prefix_count` prefixes exist. Each prefix table item comprises LE16 `size`, one byte `base_index`, then `size-1` raw infix bytes; total item padded to 4 bytes. `xattr_prefix_start` is interpreted according to placement flags: standalone when `feature_compat & PLAIN_XATTR_PFX`; otherwise within packed inode or metabox inode, requiring that data resolver and a valid special NID. **Do not assume this field is always a physical sector address.** Validate each prefix's builtin namespace and avoid recursive prefix references; if prefixed entries cannot be decoded, record metadata loss/unsupported status.

### 7.4 Name filter and image-share xattrs

The `XATTR_FILTER` optional optimization stores a 32-bit **inverted** Bloom bitmap: a 1 bit guarantees a name is absent; a 0 bit requires lookup. If `xattr_filter_reserved != 0`, skip using it. The hash convention is XXH32 over name suffix with a namespace-based seed (`0x25BBE08F` plus prefix selector), bucket modulo 32; writing this feature requires exact independent vector tests. Never skip an xattr record solely on the basis of an unverified filter in full verification. A bad filter is corrupt metadata, not a warrant to silently omit a present xattr.

`ISHARE_XATTRS` uses the indicated long-prefix entry to hold a content fingerprint, conventionally `sha256:<64 hex characters>`; it is not a filesystem-wide cryptographic integrity guarantee and must not be treated as an authenticity signature. On writer, generate it only if explicitly selected and verified against the exact post-transform logical file content.

### 7.5 fsimg metadata integration

The existing `fsimg` public metadata object represents owner/group, mode and timestamps, but **not a general xattr collection**. For reading, decode and verify xattrs internally; report `FSIMG_META_EXTRA_DATA` and per-entry metadata omissions on extraction or rebuilding. An mtree manifest currently cannot guarantee full arbitrary xattr round-trip. A future `fsimg` xattr API should use scratch-backed records/stream callbacks for name/value bytes, never allocate a per-file unbounded array. Default writer can omit optional xattrs through documented loss policy; required attributes must fail planning. SELinux labels, POSIX ACLs, Linux capabilities and overlayfs whiteout metadata are not ordinary Unix mode bits and cannot be silently translated to them. Preserve provenance of those omissions.


## 8. Compressed files: common records, algorithms and resource limits [H/I]

Unlike a SquashFS global fragment table or one stream per whole file, EROFS compression maps *logical file ranges* to independently stored compressed or plain physical extents. It permits random reads, multiple algorithms in one image, variable logical cluster sizes, large physical clusters, compressed metadata and physically shared extent contents. Compression is an **inode data layout**, not a layer to unwrap before parsing the superblock. The same mapping applies to a compressed directory or a packed/metabox inode.

### 8.1 Algorithm numbers and on-disk configuration

The algorithm identifiers in the compressed map are:

| ID | Algorithm | Stored stream / interoperability note | Reader backend idea |
|---:|---|---|---|
| 0 | LZ4 | Raw EROFS pcluster LZ4 encoding; no general LZ4 frame header. Legacy LZ4 images without `COMPR_CFGS` use the 16-bit max-distance superblock field. | Small independently authored LZ4 block decoder or optional permissively licensed LZ4 library. |
| 1 | MicroLZMA | EROFS uses **MicroLZMA** (not `.xz` or traditional `.lzma` container), with bounded dictionary and format property from config. | Current XZ Utils/liblzma decoder with explicitly supported MicroLZMA mode. |
| 2 | DEFLATE | Raw EROFS DEFLATE stream and window configuration; no gzip/zlib framing assumption. | zlib raw inflate (`inflateInit2` with negative window bits) or compatible decoder; prove exact framing with fixture. |
| 3 | Zstandard | Zstd compressed payload with EROFS-specific config limits; no assumption of whole-file frame. | libzstd with frame/window caps; independent test images. |

The initial compressed-header algorithm availability mask is at superblock `0x54` **only when `COMPR_CFGS` is enabled**. Bit `n` corresponds to algorithm `n`. In legacy layout that same field has LZ4 max-distance semantics. Compression configuration records begin **after the complete superblock including extension slots**: byte offset `1024 + 128 + 16*sb_extslots` (with each record's start rounded to 4). Records occur in increasing algorithm-ID order for each set bit. Each record consists of a **LE16 payload length** followed by exactly that many payload bytes; a zero length can signify an extended 65536-byte record in generic metadata readers, which `fsimg` must reject here unless required by a future codec. Advance to 4-byte alignment between records. The lengths below are *current known expected payload sizes*:

| Algorithm | Payload bytes | Payload fields |
|---|---:|---|
| LZ4 | 14 | LE16 `max_distance`, LE16 `max_pclusterblks`, 10 reserved bytes |
| MicroLZMA | 14 | LE32 `dict_size`, LE16 `format`, 8 reserved bytes |
| DEFLATE | 6 | `windowbits` (8–15), 5 reserved bytes |
| Zstandard | 6 | `format`, `windowlog` (encoded relative to base 10), 4 reserved bytes |

Treat unrecognized configuration versions/lengths as unsupported rather than reading beyond them; current kernel implementations may accept some forward-compatible longer records. EROFS version 1.9 utilities quote a **1 MiB maximum encoded physical cluster**, **12 MiB maximum decompressed pcluster**, an 8 MiB maximum MicroLZMA dictionary and a 1 MiB maximum Zstd dictionary/window working unit. They are interoperability limits, not a reason to allocate 12 MiB automatically under fsimg's 16 MiB minimum budget: stream compressed content to a bounded scratch buffer or declare a larger explicit resource requirement if unavoidable. The resource budget must include decompressor state and all currently pinned directory/index data.

**Padding matters:** the `LZ4_0PADDING` incompat bit changes the compressed-stream padding convention. EROFS can have compressed streams right-aligned within fixed-size allocated pclusters (zero padding at the front) or without that convention; `fsimg` must find the correct input start and exclude padding according to the encoded feature/algorithm. Test both settings. `SHIFTED` and `INTERLACED` are pseudo-algorithms for uncompressed physical payload rearrangement, not additional codec IDs in the header.

### 8.2 Common compressed map header (8 bytes)

Start `H = align_up(inode_offset + fixed_inode_size + xattr_region_bytes, 8)`. The next **8 bytes** form a compressed map header:

| Header offset | Bytes | Interpretation |
|---:|---:|---|
| `+0` | 4 | Union: LE32 fragment offset in special packed inode; low fields for inlined encoded-tail size; or LE32 low extent count for new extent map |
| `+4` | 2 | LE16 `advise` flags |
| `+6` | 1 | Low/high 4-bit compression algorithm for HEAD1/HEAD2, **or** low portion of extent count |
| `+7` | 1 | Bits0–3: `lclusterbits - blkszbits`, bits4–6 reserved, bit7: entire file stored as a fragment; **or** extent-count high byte |

Normal logical cluster size is `L=1 << (blkszbits + (h_clusterbits&0x0F))`, bounded by maximum permitted decompressed extent size and checked for shifts/overflow. If bit 7 at header byte 7 is set in non-extents mode, the entire 8-byte header must instead be interpreted as a special 64-bit fragment offset with its top marker removed; the file's bytes are sourced from the **packed inode**, and the usual map index array is absent. This special case must be resolved before interpreting `advise` as algorithm flags.

For the traditional compressed indexes, `advise` relevant bits are:

| Mask | Meaning |
|---:|---|
| `0x0001` | For compacted layout: enable 2-byte index groups. **For full-layout extents:** use the alternative encoded-extents format; meaning is layout dependent. |
| `0x0002` | HEAD1 large physical clusters; for extents also part of record-size selector. |
| `0x0004` | HEAD2 large physical clusters; for extents also part of record-size selector. |
| `0x0008` | The final encoded pcluster can be tail-packed into inode metadata. |
| `0x0010` | Uncompressed pcluster bytes can use interlaced rearrangement. |
| `0x0020` | Final pcluster can reference a fragment in packed inode. |

Index types (two low bits): `0=PLAIN` (uncompressed), `1=HEAD1` (primary algorithm), `2=NONHEAD` (continuation), `3=HEAD2` (secondary algorithm). **HEAD1/HEAD2 label a cluster's compressor choice; they are not file types.** The logical start of a compressed extent may fall in the middle of a logical cluster (`di_clusterofs`), so never assume each physical compressed cluster starts on an `L` boundary in logical file space.

### 8.3 Traditional full compressed-index format (layout 1)

In the legacy full-index variant, leave **8 reserved/padding bytes after the map header** and place 8-byte records at `H+16 + 8*lcn`. A record has:

| Relative offset | Bytes | Semantics |
|---:|---:|---|
| 0 | 2 | LE16 `di_advise`: low 2 bits = type, bit 15 = partial reference for head records |
| 2 | 2 | LE16 `di_clusterofs` for head/plain records |
| 4 | 4 | Union: LE32 physical start block on head; **two LE16 delta values** on NONHEAD |

A HEAD1, HEAD2 or PLAIN starts at logical `lcn*L + di_clusterofs` and selects a physical pcluster at `startblk*B`. A NONHEAD record's first delta tells how many logical clusters to look back to reach its controlling head, and its second delta indicates look-ahead toward the next head. The first NONHEAD after a big-pcluster head can carry a `0x0800` marker in its backward-delta field, in which case low remaining bits indicate the **compressed block count** and the backward distance is effectively one. This disambiguates multi-block physical pclusters. Invalid zero/backward references, huge values or cycles must be rejected. A head without a big-pcluster indicator generally consumes one physical block (or a special inline/fragment extent), **not** one logical cluster of data.

**Mapping a logical byte offset** in this variant (independently derived high-level algorithm, not a source-code transcription):

1. Reject `offset >= i_size`; calculate `lcn = floor(offset/L)` and the offset within that logical cluster; fetch its bounded index.
2. If the entry is a head/plain and the byte is at or beyond its logical start, select it. If the byte precedes the head's intra-cluster start, resolve the preceding physical cluster; for NONHEAD, follow validated backward distances until a head is reached. Put a finite upper bound on iterations derived from the number of relevant index slots.
3. Establish the logical start `logical_head = head_lcn*L + clusterofs`, and locate its right boundary by walking forward to the next head (respecting saved lookahead distances and any permitted last-EOF special extent). Clamp logical length to `i_size`. This is the output range of the decompressor, which may be greater than `L`.
4. Derive stored byte offset/length: normally `startblk*B` and one `B`; for big pcluster read the explicitly encoded compressed block count. If tail-packed, derive its metadata offset and size; if a fragment, switch to packed-inode logical offset instead.
5. Verify every arithmetic operation, source/device selection, stored input span and claimed decoded bytes. Decode *only the necessary pcluster* into bounded scratch, then select the caller's subrange. Never execute a decompressor against an unchecked pointer or trust the decoded length inside the stream.
6. A `PLAIN` head is handled by the corresponding shifted/interlaced raw transform, not by running an LZ4 decoder.

The `i_u` union of a compressed inode is not a flat `startblk`, and the trailing `i_size` is logical uncompressed size. Mapping metadata is not itself compressed unless the inode lives in the metabox.

### 8.4 Compacted compressed index (layout 3)

This layout trades more complicated index math for fewer metadata bytes. Following the same 8-byte header at `H`, the remaining map consists of packed groups of **4-byte amortized entries** or, with `COMPACTED_2B`, a central area of **2-byte amortized entries**, surrounded by 4-byte areas to satisfy alignment. The grouping is *not simply an array of independent 2- or 4-byte LE integers*; the group contains a **LE32 physical base block** and bitfields for logical offsets/types, from which other heads' block addresses are reconstructed.

Important interoperable grouping facts from the current parser:

- `4B` amortized packing uses **2 logical records in an 8-byte group**, with one 4-byte physical block anchor and the remaining 32 bits used for two logical-record bitfields.
- `2B` amortized packing uses **16 records in a 32-byte group**, again with a 4-byte physical block anchor and 28 bytes of bitfields.
- The 2B scheme is permitted only where logical cluster bits <=12; the 4B scheme supports logical cluster bits <=14. Larger logical clusters should use full or encoded extent indexes.
- The initial run of 4B entries aligns the middle 2B region to 32 bytes; only complete 16-entry groups enter the 2B region; remaining records use 4B groups. Group alignment and final partial pack are **nontrivial**. Derive group boundaries from the record count and header byte offset, not by `index * 2` alone.
- For each packed entry, extract `lobits = max(lclusterbits,12)` low bits and then 2 type bits at the appropriate bit offset. In 4B groups, each field uses `(8*8-32)/2 = 16` bits; in 2B groups `(32*8-32)/16 = 14` bits.
- In NONHEAD entries, the low bits encode a backward distance, except the last slot of a group encodes a forward-distance-derived value. This exception must be tested across pack boundaries. Big-pcluster compressed-block counts reuse the marker `0x0800`.
- Reconstruct each head physical block by accounting for the number of previous heads/physical blocks from the group's anchor. With big pclusters, head weights are variable and must be summed, not simply counted.

**Recommended implementation strategy:** First develop a small **independent normalized mapping model** (a bounded iterator that yields logical start/length, storage address/length, algorithm and source) and validate it using *full-index* fixtures. Add one function that decodes a compacted group to those normalized values, with explicit bit-position tests for group-start, group-end, last-slot lookback, 2B/4B boundary, mixed HEAD1/HEAD2, big pcluster and EOF. Compare every returned mapped region to `dump.erofs -e` and a whole-file SHA-256 of `fsck.erofs --extract`. Do not optimize compressed random lookup with an unchecked negative index or a process-global cache.

### 8.5 Compression selection and per-file mixing

`h_algorithmtype` contains two nibble algorithm IDs for HEAD1 and HEAD2. Their availability must be checked against the superblock algorithm mask and supported codec build options. A file can contain PLAIN, HEAD1 and HEAD2 pclusters; a directory or special metadata inode can also be compressed. An omitted codec is not an excuse to report an empty file: `fsimg` should expose a distinct unsupported-codec error, and a full verifier should fail if it cannot decode required payloads. Enable compression algorithms incrementally; it is acceptable for the first reader to support compressed metadata **structure verification** without claiming to verify decoded payloads.

### 8.6 Encoded byte-oriented extents: alternative `COMPRESSED_FULL` [E/H]

The newer `48BIT` layout introduces an **extent list instead of logical-cluster backward links**. For `COMPRESSED_FULL` with `advise & 0x0001`, interpret bytes `H+0..H+5` as a **48-bit extent count** (`h_extents_lo` + `h_extents_hi << 32`). The extent-record size is `4 << ((advise >> 1) & 3)` = **4, 8, 16 or 32 bytes**. Records begin at `align_up(H+8, record_size)`. The choice changes which fields are explicitly stored; a 32-byte record has:

| Extent offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 4 | `plen`: encoded physical length and format/flags |
| 4 | 4 | `pstart_lo`: physical **byte** offset low 32 bits |
| 8 | 4 | `pstart_hi`: physical offset high 32 bits |
| 12 | 4 | `lstart_lo`: logical **byte** offset low 32 bits |
| 16 | 4 | `lstart_hi`: logical offset high 32 bits |
| 20 | 12 | Reserved |

Not all fields exist in 4/8/16-byte encodings, and the smaller encodings use **implicit location rules**, not zero-filled omitted fields. Current user-space behavior distinguishes (a) compact records with a leading physical position and cumulative stored lengths, (b) fixed logical-cluster-spacing records with shorter per-record positions, and (c) explicit logical-position records searched by binary search. Do not treat the 4B variant as 4B truncation of the 32B structure. `plen` low **21 bits** form the encoded byte length (`(2*1MiB)-1` mask), bit **27** denotes partial/shared reference, bits **28–31** encode algorithm format: zero means raw shifted/interlaced special handling; otherwise `algorithm = fmt - 1`. Extent starts are sorted by logical byte offset; logical length is until the next extent start or EOF. For the final fragment extent, payload fields instead encode an offset into the special packed inode (conditional on the fragment feature), not an ordinary physical range.

**Safety:** A compressed extent length must not exceed current supported pcluster bounds or the actual backing slice. Zero-length encoded extents require extension-specific interpretation; don't manufacture holes from them. Establish 48BIT byte-position records against `dump.erofs` and matching `mkfs.erofs -E 48bit` fixtures before claiming reader/writer support. The feature is experimentally marked by upstream in 2026 and may change; pin a release/tag in tests.

### 8.7 Inline compressed tail / ztailpacking [H/I]

`ZTAILPACKING` enables a compressed file's final pcluster to be stored near its inode rather than consuming a full external block. In the map header, bytes `+2..+3` can encode the size of this tail payload. The last head is discovered through index/EOF semantics, and its stored input location is inside the metadata block after its compressed map. Reject if the encoded tail spans the metadata block boundary or overlaps an unrelated inode. The final decoded stream can represent a partial lcluster; no expectation that decoded length equals a full cluster.

Uncompressed `FLAT_INLINE` is **not** ztailpacking; they are different per-inode data layout mechanisms. Add regression fixtures with 1-byte, B-1, B, B+1, multi-block+tail, tiny symlink and long-xattr cases, to catch interactions between metadata and compressed-tail alignment.

### 8.8 Packed fragments, all-fragments, dedupe and shared physical extents [H/I]

A filesystem can have `packed_nid` identifying a special inode whose **logical file data** is a repository of byte fragments from regular files. A compressed file's final cluster may point at a fragment offset instead of its own dedicated pcluster. If the high marker in the 8-byte compressed header signals all-fragments, the entire file comes from a packed-inode range. An encoded-extents terminal record has its own way of carrying the fragment offset. The packed inode can itself use compressed file data, and the reference must therefore go through its *normal content resolver*, not by assuming packed-inode data is a contiguous raw disk span.

- Validate `packed_nid` before use and forbid packed-inode self-reference/circular references.
- Check `fragment_offset + requested_length <= packed_inode.i_size` using overflow-safe arithmetic; fragments can be unaligned.
- A physical pcluster shared by dedupe is not itself a hardlink: two different file inode IDs can point at overlapping/identical compressed physical ranges.
- If a pcluster encodes a **partial reference**, decompression can produce a larger byte interval than a file consumes; validate the stored algorithm and logical slice. Do not assume shareable extents are independently owned or permit a writer to overwrite one after referencing it.
- A writer's dedupe dictionary should use a cryptographic hash as an *index* only, with final byte equality checked before sharing; a digest match alone is not byte-equality proof.

The current 1.9.4 `erofs-utils` release specifically fixed extraction for fragment-enabled images, so using an older `fsck.erofs` or `dump.erofs` as the sole fixture oracle is unsafe. Test exact tool versions, prefer both FUSE/kernel and a byte-level independent expected file digest.

## 9. Chunk-based data, holes and external devices [N/H/P]

### 9.1 Chunk format

An inode with datalayout 4 requires `feature_incompat & CHUNKED_FILE`. Interpret the LE32 at inode `+0x10` as a **chunk summary**. The low 5 bits give `chunkbits` *above filesystem block size*; bit 5 (`0x20`) selects 8-byte indexed entries over a simple 4-byte map; bit 6 (`0x40`) enables higher addressing in the newer layout; all other bits are currently reserved. `chunk_size = B << chunkbits`, with checked shift and an upper-bound appropriate to the data/byte address space.

Number of entries: `ceil(i_size/chunk_size)`. The first index begins at **`align_up(inode_offset + inode_fixed_bytes + xattr_region_bytes, entry_width)`**. Each entry describes one *logical* chunk. The last chunk may have less than `chunk_size` actual content, and the reserved physical span may still be block-rounded.

**4-byte entry:** LE32 `startblk` only. Without extra devices, it names a primary physical block. With a device table, it is in the **unified block address space** and must be resolved to the device whose `[uniaddr, uniaddr+blocks)` range contains it; translated device block address is `startblk-uniaddr`. No device ID is encoded.

**8-byte indexed entry:** `[LE16 startblk_hi, LE16 device_id, LE32 startblk_lo]`. In non-48-bit images, ignore/reserve high address bits per format; for 48-bit, combine the low32 and high16. `device_id=0` means unified address translation; nonzero means **that secondary device** (device ID starts at 1). Mask only the documented device-ID flag bits if enabled by a particular version; otherwise values beyond `extra_devices` are invalid. The all-ones 32/48-bit block address is a **hole**, read as zeros. Do not report a missing external device as a hole.

### 9.2 Extra-device table

When `DEVICE_TABLE` is enabled, the primary image contains `extra_devices` records at byte offset `devt_slotoff*128`. Each record is **128 bytes**:

| Record offset | Bytes | Meaning |
|---:|---:|---|
| `0x00` | 64 | Opaque `tag` identifying the data source for userspace; the kernel does not interpret its contents |
| `0x40` | 4 | LE32 `blocks_lo`, number of blocks on device |
| `0x44` | 4 | LE32 `uniaddr_lo`, unified-address start in blocks |
| `0x48` | 2 | Under 48BIT, high16 `blocks_hi` |
| `0x4A` | 2 | Under 48BIT, high16 `uniaddr_hi` |
| `0x4C` | 52 | Reserved |

All device table reads and each target extent must be validated against **their respective backing files' real lengths**. Validate that unified address ranges do not overlap ambiguously and are representable. External-device tags are identifiers only; they must never be automatically dereferenced as host paths, URLs or S3 object names from untrusted images. This is especially important for reading images shipped in OCI or on removable media.

### 9.3 `fsimg` external-source API proposal

Today `fsimg`'s reader has one input I/O object for a volume. Add a **read-only externally supplied data-source array** only if/when implementing multi-device profiles: the caller binds `device_id` or a documented tag to a positional immutable `fsimg_io_t` object, with its own byte-size bound, lifecycle, immutable snapshot and resource accounting. A CLI might permit repeated `--erofs-blob path` options, in slot order, with an explicit `--erofs-blob-tag=tag:path` mapping only if tag semantics are defined by caller and never trusted from image alone. This needs common API review; *do not fake the feature by opening files from a tag automatically*. Without the supplied input, opening a metadata-only image may work for listing; reading referenced external bytes must return `FSIMG_EUNSUPPORTED` or a specific missing-device report rather than `EIO` masked as zeros.

### 9.4 Tar-index/OCI and layer use

`mkfs.erofs --tar=f` creates a standalone filesystem from a tar stream. `--tar=i` and `--tar=headerball` produce metadata-only references requiring separate file-data blobs; they are not self-contained raw filesystems. Native EROFS container layers and `go-erofs` `MetadataOnly` similarly separate metadata from content and may use overlay whiteouts/opaque directory xattrs. An EROFS filesystem image can also be transported with an OCI EROFS-specific media type. Those **transport and layering semantics are not new EROFS inode formats** and should live in `fsimg`'s existing layers stack, not be mixed into core on-disk reader logic. See [EROFS image layer specification](https://github.com/erofs/erofs-image-spec) (Apache-2.0) and [`go-erofs`](https://github.com/erofs/go-erofs) (Apache-2.0).

## 10. 48-bit addressing and modern encoded extents [E]

The EROFS `48BIT` incompat flag `0x80` (experimental in the 2026 utilities) broadens block addresses across several structures, widens special NID semantics, adds individual seconds deltas to compact inode timestamps, and enables optional dot omission. It **does not** magically reinterpret every 32-bit field as 48-bit without context.

- Primary filesystem block count: low 32 bits at super `0x24`, high 16 bits stored in the former `rootnid_2b` slot (`0x0E`) when the eight-byte root NID at super `0x70` is present. This means a reader must choose its root NID and block count interpretation as a single feature-specific decision.
- Inode `startblk`: combine the LE32 low component at inode `0x10` with high16 in `i_nb` at inode `0x06` **only in formats/kinds that permit it**. Compact non-directory inodes can use bit4 as `NLINK_1`, effectively hardlink count 1 and reclaim the 16-bit nlink slot for high address. Extended inode variants can use high bits when allowed.
- Compact timestamp: the formerly reserved core word at inode `0x0C` is interpreted as LE32 `i_mtime` seconds delta added to super `epoch`, with nanoseconds from `fixed_nsec`. A conforming non-48BIT image must leave this reserved word zero (even though current upstream code permissively decodes it without checking the flag).
- Directory: a format-specific bit permits omitted `.`; synthetic dot should not be mistaken for corruption. `..` and the directory graph still require validation.
- Chunk index: 8-byte entries gain `startblk_hi`; table `blocks_hi` and `uniaddr_hi` at slot offsets `0x48`, `0x4A`.
- Compressed full indexes may use **byte-addressed** encoded extents, with record widths 4/8/16/32 bytes (Section 8.6), permitting more compact indexes for unaligned compressed storage.
- `rootnid_8b` and `metabox_nid` must not be mistaken for ordinary file data block numbers; a special NID high bit selects an alternate address domain.

**Implementation prerequisite:** first make all internal arithmetic 64-bit checked, explicitly tagged as byte offset vs block address vs NID slot. Do not use a u32 path internally and truncate at the driver boundary. Full read support requires separate fixtures proving each widened field, especially a synthetic image with startblk >= 2^32 (use sparse files to avoid petabytes of allocation, or input stream virtualization) and NID > 65535. Do not require a giant real backing file for the unit test if a bounded virtual IO fixture is possible.

## 11. Metabox and compressed metadata [E/H/I]

The `METABOX` incompat flag `0x100` designates a **metabox inode** whose *uncompressed logical contents* form an alternative metadata address space. The special inode NID is at superblock `0x80` when an extension slot is present; its own on-disk inode still resides in the ordinary primary metadata domain. Other directory entries can set **bit 63 of the target NID** to select this alternate domain: remaining 63 bits are the slot offset in the decoded metabox contents (32-byte units). Read such an inode by mapping the metabox inode's logical bytes, not by multiplying an apparent NID by 32 in the primary file. The packed inode must not refer recursively to itself via metabox bit 63, and metadata decoding needs a bounded recursion guard.

Shared xattrs can also be relocated there by `SHARED_EA_IN_METABOX`; prefix tables can reside in the special inode as configured. This creates a **dependency graph**, not merely compressed directories: superblock → base metabox inode → decoded metabox file ranges → referenced inode metadata/xattrs → ordinary file content. A reader should provide a distinct `metadata_read(domain,offset,length)` accessor with two implementations: original bytes through bounded primary volume I/O and metabox logical bytes through the *same* verified file-content/decompression resolver. The accessor must be iterative/reentrant-safe: prohibit using metabox-hosted metadata to look up the metabox inode itself, cap nesting depth, and charge decoded metadata to the same context budget as file data.

The 2026 `mkfs.erofs` gained `-m` / `--MZ` style options for metadata compression and isolated metadata zones. The current upstream release advertises both **experimental 48-bit layout and experimental metadata compression**; user-space code explicitly warns that there is no efficient cache for metadata-compressed reads. Accordingly:

- Reader first: inspect header, report metabox NID and the unsupported flag; optionally permit reading *only* data known not to require metabox records if public semantics permit, but full verification must not claim completion.
- Next: independently implement `metadata_read` over a decoded special file and support uncompressed metabox.
- Then: enable compression of the metabox file once compressed extent reading and recursion safety are mature.
- Writer: not until stable upstream writers and kernel versions agree on binary layouts, compression settings and repeatable emitted images.

As with all decoded metadata, trust neither the stored metabox logical length nor any NID offset until both are checked against the selected logical data length. A malformed metabox cannot force an unbounded scratch allocation, recursive seek chain or cache blowup.

## 12. Filesystem integrity and verification scope [P]

EROFS's **superblock checksum is not whole-volume integrity protection**. EROFS generally has no per-file cryptographic hash chain in its core format. Reproducibility hashes, image-share xattr digests, OCI descriptor digests or dm-verity are different layers and must be reported as such. `fsimg` verification should never claim cryptographic authenticity without an independent trusted checksum/signature input.

**Three useful verification depths:**

1. **Header / mountability:** check EROFS identity, superblock size, valid block size, supported incompatible flags, CRC32C when enabled, root inode bounds and directory type. Report volume and feature summary.
2. **Metadata/full-tree:** enumerate all reachable entries once by path, validate directory blocks and edges, inode field legality, timestamps and types, hardlink/NID consistency, inline/shared xattrs, chunk indexes, compression indexes/encoded extents, source/device bounds, reserved fields according to strictness policy, and special inode references. Detect graph cycles. This does **not** promise that compressed payload bytes can be decoded if codecs are disabled.
3. **Full payload:** every reachable regular file and symlink (plus directories, packed inode and metabox as required) must be read to its logical length, decompress/check every referenced pcluster, verify data maps and I/O coverage, check no extent leaves the intended device, and account logical/stored bytes. Size, content CRCs and any optional independently supplied expected digests are checked. A file count alone is not evidence that all bytes were read.

Proposed report fields: volume UUID/label, block size, inode count (advisory) vs reachable entries, directory count, flat and compressed file counts, chunked files, packed-fragment references, metabox uses, external-device refs, logical bytes, referenced stored bytes, unique referenced compressed extents (best effort), unsupported codec count, unknown compatible flags, metadata omitted count, dangling/cyclic references and first failing byte-level location. Distinguish *observed extents* from unreferenced allocations, since EROFS has no canonical global free-space map; full verification of the reachable tree is not equivalent to enumerating all bytes in the image.

**Error taxonomy:** not EROFS signature → `FSIMG_EFORMAT`; EROFS claimed but an unsupported feature/codec/layout/required blob → `FSIMG_EUNSUPPORTED`; illegal in-bounds format relationship, checksum mismatch, malformed record, cycle or unexpected EOF from the claimed image range → `FSIMG_ECORRUPT` (or bounded underlying `EIO` when actual I/O fails); CPU/memory/time/work-budget exceeded → resource error, **never** masquerading as corrupt. Name safety and sandbox policy are common layer responsibilities. In salvage mode, recover only complete independently verified logical content prefixes and count omitted/lost data; never emit guessed bytes from a broken compressed pcluster.

## 13. Proposed `fsimg` source layout and driver behavior [P]

Use a single public variant (`erofs`) and place the implementation in a new `lib/formats/erofs/` directory. The following file organization is **a proposal**, not an instruction to add these files before implementation work is approved:

| Proposed file | Responsibility | Existing fsimg seam |
|---|---|---|
| `descriptor.c` | Bounded prefix probe, single variant capabilities, registration | `lib/formats/format.h`, `format.c` |
| `erofs.h` | Private decoded types, typed offsets, on-disk facts/constants, no packed pointer casts | `lib/core` integer codec utilities |
| `super.c` | Header, feature negotiation, CRC, device-table descriptions | Driver `open` |
| `inode.c` | NID address and metadata-space access, compact/extended decoder | Driver `replay`, metadata |
| `directory.c` | Logical directory block parser, bytewise name ordering, parent and type validation | Driver `prepare` and keys |
| `xattr.c` | Inline/shared attrs, long prefixes and Bloom/filter interpretation | Metadata loss / verification |
| `data.c` | Uncompressed flat/chunk extents and source/device bounds | Driver `resolve` |
| `zmap.c` | Full/compact index decoder, new extent records, normalized logical map | Reuses `resolve` |
| `decompress.c` | Codec dispatch, bounded held-buffer / spool decoding, framing | `lib/codec`, session resource budget |
| `packed.c` | Special packed fragment inode content lookups | `data.c` and `zmap.c` |
| `metabox.c` | Alternate metadata address domain using decoded metabox file | `inode.c`, `data.c` |
| `verify.c` | Header, metadata-tree and data-payload validation | Driver `verify` |
| `write-plan.c` | Stable inode allocation, directory/xattr layout, data extent planning, volume geometry | Writer `plan` |
| `write-emit.c` | Byte encoding, zeroing/padding, source copying, CRC, final validation | Writer `emit` |
| `test/erofs.c`, `test/erofs-write.c` | Targeted positive/negative format witnesses | Existing test structure |
| `test/spec/erofs/` | Independent Ruby models/fixtures/provenance manifests | `specs/design-spec-testing.md` |

The driver should use **internal mathematical types**, even if these are all `uint64_t` in C: `erofs_block_t` (block number), `erofs_nid_t` (32-byte metadata slot), `erofs_fileoff_t` (logical byte offset), `erofs_phys_off_t` (physical byte offset) and `erofs_device_id_t`. Explicitly perform `checked_mul(block,B)`, `checked_add(phys,length)` and `checked_shl` at conversion boundaries. Keep the primary image's `fsimg_io_t` separate from independent optional device sources. Make `erofs_map_data()` return normalized records:

| Normalized extent field | Meaning |
|---|---|
| `logical_start, logical_length` | Complete byte interval this mapping covers |
| `source_kind` | `primary_raw`, `extra_device`, `known_zero`, `inline_metadata`, `packed_inode`, `compressed_pcluster`, `metabox_data` |
| `source_id` | Device or special inode reference, never an implicit host file path |
| `physical_start, stored_length` | Byte range in selected source (or logical offset inside special inode) |
| `codec_id, decoded_span` | Compression algorithm and decoded-output interval when applicable |
| `flags` | Tail, dedupe, partial reference, interlaced, etc., validated per layout |

A single map object allows `read_at()`, `next_range()`, extraction, `xOf` and `Wdf` to share correctness decisions. It does **not** mean `fsimg` needs to store every extent of a giant inode in RAM. Use a bounded read-ahead cursor and seekable scratch-backed extents, with a small fixed-size LRU cache keyed by `(image identity, inode NID, logical cluster or extent-record index)`. When compressed record lookup needs the preceding head, use bounded backward traversal or a small anchor index; never let attacker-controlled deltas create O(file_size²) work without session charging.

### 13.1 Driver capability advertisement

With the current `fsimg` metadata/kind definitions, expected reader kinds are regular file, directory, symlink, FIFO, character device, block device and socket. Current writer input can create regular files, directories, symlinks and FIFOs; device nodes/sockets must remain reader-only unless the common writer catalog is extended. With the first writer profile, limit to regular files/directories/symlinks/FIFOs, owner/mode and a chosen mtime policy. `FSIMG_META_EXTRA_DATA` is *read-report only*; xattrs cannot be written losslessly from current common metadata. Declare `max_name` as 255 raw bytes, `max_label_bytes` 15, block-size choices as supported sector/allocation values, and max file/volume sizes from **implemented** address variants rather than the 64-bit specification maximum. The `min-volume-bytes` value must be derived from actual superblock, root inode, directory metadata and alignment; EROFS has no FAT32-style fixed minimum.

### 13.2 Semantic edge cases with fsimg's public API

- **Case-sensitivity:** EROFS uses raw filename bytes; preserve names in listing and validate common name profiles. Do not adopt FAT/HFS normalization or case folding.
- **Root:** the root inode is not necessarily NID zero, and its `i_ino` need not equal NID. Root is expected to be a directory; `fsimg` should present root consistently even if `.` is synthesized.
- **Hardlinks:** model same-NID multiple paths as one underlying inode. If the public writer catalog lacks a link-to-existing-inode relation, record a metadata-loss limitation and never claim hardlink fidelity. Readers can still report per-entry data while checking `i_nlink` consistency.
- **Sparse data:** EROFS chunk hole indexes, optional null flat pointers and zero-generating indexes must map to known-zero spans through `resolve` rather than copying arbitrary physical zeros.
- **Symlinks:** the on-disk target is ordinary logical file content. `fsimg`'s common target helper can be used when the driver explicitly decoded a bounded target; otherwise a symlink can use the content extent resolver if allowed by the common contract. Never follow the target during extraction/source traversal without the common safety policy.
- **Compressed directory:** `prepare` must read from the generic content resolver, not assume `directory_startblk*B` for all directories. Even before compressed directories are a writer feature, the reader should be architected to handle them.
- **Metadata loss:** SELinux labels, ACLs, capabilities, hardlink identity, nonrepresentable timestamps, device nodes, xattrs and special file types must be accounted for under `specs/design-metadata-loss.md` and `specs/design-device-nodes.md`, not dropped behind a successful extraction status.
- **System area:** preserve boot/partition bytes outside the volume when a layer is involved. Creation of bare EROFS can zero the first 1024 bytes; the rest is driver-owned.
- **Forced format:** an explicit `-F erofs` should select creation profile, and a force-open of an actual non-EROFS image follows the standard `fsimg` identity-waiver rules; it is not a promise to parse any arbitrary bytes.

### 13.3 Open, prepare, replay and resolve behavior

The reader's `open` performs bounded superblock decoding and sets the volume info. `prepare(root)` opens the root inode and its logical directory stream, admitting children exactly once. `prepare(child)` uses the currently selected NID; directory data is decoded in block-size windows. `replay` validates the selected directory entry, decodes its inode and fills the `fsimg_entry_t` snapshot. `resolve` chooses a mapping record for one logical file offset and returns an extent containing that offset: raw mapped extent, known-zero extent or held decompressed byte range owned by the driver. Because `reader->held` is ephemeral until the next resolver/walk advance, a compressed cluster cache must not return memory that can be invalidated before the caller copies from it. The common session handles memory/work/time and input-identity checks; format code charges any additional decompression and metadata enumeration work.

Do not perform full-tree recursive preloading on open; it would violate large-tree scaling and `fsimg`'s streaming convention. No global inode hash table is necessary for ordinary reads. For verification and detection of cycles, use scratch-backed visited-NID sets and a bounded work queue.

### 13.4 Writing and the existing two-pass planner

`fsimg` writers collect a catalog and then plan+emit, so EROFS fits well. Preserve source repeatability and never read or modify arbitrary source paths from a tar/OCI tag. A writer can decide upfront, for each inode, whether it is compact (fits <=32-bit size, 16-bit UID/GID and legacy timestamp behavior) or extended. Plan all data and metadata before emitting; a later size increase cannot relocate already-written items. Compute inode offsets and therefore NIDs deterministically after the layout plan. Because directory entries must contain child NIDs, plan the entire inode table before directory payload writing, or spool unresolved directory records with patched NIDs to a scratch-backed plan; never depend on host directory iteration order in reproducible mode.

## 14. Concrete standalone EROFS writer specification [P]

### 14.1 Baseline output profile `erofs` (no experimental bits)

- Filesystem blocks: `B=4096` default. Optionally support B=512/1024/2048, but only after native reader tests on kernel versions that support them. Avoid sector size separate from EROFS block size unless host/disk stack requires it.
- Superblock: base 128 bytes at offset 1024; magic set; `SB_CHKSUM` and `MTIME` compatible bits; **zero incompatible feature bits** for basic uncompressed images. `dirblkbits=0`, `available_compr_algs=0`, no extra devices, no packed/metabox inode.
- One or more contiguous metadata blocks beginning at `meta_blkaddr`; inodes align to 32B. Arrange inodes by stable directory traversal (e.g. bytewise path order), not by nondeterministic filesystem enumeration. Minimize wasted metadata blocks while keeping every inline payload in bounds.
- Root inode pointed to by `rootnid_2b` if it fits 16-bit. If it cannot fit, either reorganize metadata placement or refuse this baseline profile with `FSIMG_EUNSUPPORTED`; do **not** silently enable the experimental 48BIT feature in a supposedly core profile.
- Inode selection: choose 32B compact only when metadata representation would be exact (size <=2^32-1; UID/GID <=65535; link count <=65535; modification time exactly equals the shared `(epoch, fixed_nsec)` of this **non-48BIT baseline writer**). Otherwise 64B extended. Explicit `i_format` layout 0 or 2; all reserved fields zero. `i_ino` stable and unique per underlying inode; hardlinked names share one NID.
- Files and symlinks: use inline tail where the inode body has enough contiguous remaining block bytes, otherwise flat/plain blocks. For full-block payload, stream from source; do not require image-sized data staging. Use same mapping for symlink target.
- Directories: pack 12B entries and concatenated raw names in ascending bytewise order, choose block break by exact available record+name space. Include `.`/`..` in baseline; select entry file types accurately. The size in directory inode is the **sum of valid directory-block contents**, with final partial block handling matched to published core rules.
- Attributes: optionally start with no xattrs; when xattr API exists, inline first, shared-area dedupe second. `MTIME` advertised only when timestamp semantics actually preserved. Document missing atime/ctime separately.
- Alignment: all physical data blocks B-aligned, all inodes 32B-aligned, all xattrs 4B-aligned, no overlap, no uninitialized bytes. File holes must return zeros; output may be sparse on disk, but the virtual image length is fixed by the plan.
- Image time/UUID/label: honor `-R`, `-T`-like caller policy, deterministic creation time and deterministic UUID or caller-specified identity. Use zero padding and stable inode ordering; no host/time/randomness leaks into reproducible output.
- Verification: after emission and superblock checksum, `fsimg Wdf`, `fsck.erofs` and `dump.erofs -s` must all accept the image. Mount with Linux or FUSE where available and compare file data against the source tree.

### 14.2 Byte placement plan

A legal simple layout is **not uniquely specified** by EROFS, so `fsimg` may choose whichever allocation order minimizes size and enables one-pass final emission. A particularly easy layout for a first writer:

1. Reserve `[0, 1024)` and the superblock-bearing block. Record final label, UUID, feature flags and checksum placeholder.
2. Reserve a contiguous metadata zone at a B-aligned `meta_blkaddr`; assign all inode NIDs in ascending 32-byte slots with 64-byte inodes taking two slots. Reserve per-inode inline xattrs and inline tails when selected.
3. Calculate each directory stream size, including all child NIDs and full records. The directory stream can be placed in external full blocks or inline as a tail, just like file data.
4. Reserve B-aligned regular file full blocks after metadata. Optionally choose placement so files of identical content share blocks only when dedupe is explicitly enabled; baseline can emit a copy per file.
5. Ensure no planned physical extent shares bytes with mutable metadata or another emitted extent unless deliberate immutable dedupe.
6. Emit zero-initialized image including leading reserve and all record padding. Write inode bodies, xattrs, directories, payloads and superblock. Compute CRC32C with the final feature bits and final bytes (zero checksum field during CRC), write checksum last. Finalize by verifying output extent ranges and publication invariants before common code commits it to destination.

A tighter size profile may interleave small file metadata and data, but it is not necessary for correctness. A **bounded spill-backed planning stream** is preferred over an all-in-memory inode tree: its per-entry rows hold `(catalog_id, NID, size, inode_variant, file_type, stored_extent, xattr_ref, name sorting key)` and the entire plan is stable after sealing. Avoid a pointer graph proportional to the filesystem size.

### 14.3 Exact writer sizing formulas

For each inode, `fixed = compact ? 32 : 64`; `xattr = (icount == 0) ? 0 : 12 + 4*(icount-1)`; `slot_count = ceil((fixed+xattr+inline_tail+alignment_slack)/32)` when a contiguous slot reservation is used. However, the **wire format does not encode a variable inode slot count**: the writer can allocate gaps and choose NIDs such that each inode's fixed+variable data fits without overlapping another inode; it must not assume a raw packed sum is a valid NID step if a metadata block edge truncates the tail. For each directory block with `N` entries and name lengths `len[i]`, usable data is `12*N + sum(len[i])`, plus any last-name terminator/padding needed; require <= directory block size and ensure names' individual `nameoff` fit LE16. For raw full blocks, stored bytes = `ceil(file_full_bytes / B)*B`; for inline suffix, tail bytes = `S % B` and must fit after metadata in the same B-sized block.

For minimal size (`-M`), try legal B values, inode variants, inline-tail choices and placement order deterministically, comparing total image bytes under the exact selected features and geometry. `-M` should never quietly change compression algorithm/feature set or turn on 48BIT for size reduction unless the CLI exposes such a policy explicitly.

### 14.4 Subsequent writer features

| Feature | Planning effect | Output validation |
|---|---|---|
| Shared xattrs | Deduplicate identical name/value entries into shared pool; write LE32 units-of-four indices; long-prefix table as optional separate region | `fsck.erofs --xattrs`, independent xattr parser |
| Chunk indexes | Split source into fixed chunks, map to chosen physical blocks; optionally dedupe identical chunks; emit 4B or 8B indexes | `dump.erofs -e`, `fsck.erofs --extract`, compare file SHA256 |
| LZ4 compression | Break data into logical/pcluster units, compress to bounded scratch, build full indexes first, choose physical placement | `mkfs.erofs` reader, `fsck.erofs`, kernel mount |
| Compact indexes | Encode 4B/2B mixed groups; more complex plan requires exact group anchoring | Independent index decoder, random reads across groups |
| Compressed tails/fragments | Allocate inline final pclusters or packed-inode offsets; sharing/sizing must precede final inode layout | `erofsfuse`, fragment fix present in erofs-utils >=1.9.4 |
| Multi-device | Plan a pair/group of outputs atomically; stable device slot order, tags and external bytes | All blobs supplied to native checker and `fsimg` |
| Metadata compression | Build decoded metadata domain, then encode as metabox inode; avoid circular NID dependencies | Require pinned kernel/erofs-utils latest 2026 |
| 48BIT | Choose high bits, rootnid_8b and encoded byte extents; never mix with classic core-only profile | Sparse virtual images and Linux >=6.15 |

**Do not enable LZ4HC, LZMA, DEFLATE or Zstd compression through `fsimg` merely because external `mkfs.erofs` can emit them.** The `fsimg` writer needs its own freely distributable compression backend, deterministic output policy and validation. It may expose algorithm choices only once these requirements are satisfied. Compression of external devices or metadata requires a reproducibility story for both inputs and outputs.

## 15. Ready-to-use native testing and fixture commands

Commands in this section use **existing external utilities**; they do not presuppose an EROFS `fsimg` driver is already implemented. They should be run on Linux for full kernel-mount tests, while portable utilities and the eventual fsimg driver should run on macOS and FreeBSD as well. Build `erofs-utils` from a pinned 1.9.4 tag or newer verified tag with the relevant compressor backends; record `mkfs.erofs -V` (or `--version`), package build configuration, Linux kernel version and image SHA256 in fixture provenance. Some distro builds omit LZMA, zstd or FUSE support. Avoid Docker as a requirement; plain executable utilities and QEMU/VM are sufficient.

### 15.1 Independent writer → fsimg reader (baseline)

```sh
set -eu
mkdir -p fixtures/tree/etc fixtures/tree/bin fixtures/tree/emptydir
printf 'hello erofs\n' > fixtures/tree/etc/message
printf '\000\001\002\003\377\n' > fixtures/tree/bin/binary
: > fixtures/tree/empty
ln -s ../etc/message fixtures/tree/bin/shortcut

# Reference writer: core/uncompressed EROFS, 4096-byte blocks.
mkfs.erofs -b 4096 -U clear -T 0 fixtures/core-4k.erofs fixtures/tree

# Reference read/check tools:
dump.erofs -s fixtures/core-4k.erofs
fsck.erofs fixtures/core-4k.erofs
mkdir -p fixtures/extracted-core
fsck.erofs --extract=fixtures/extracted-core fixtures/core-4k.erofs
cmp fixtures/tree/etc/message fixtures/extracted-core/etc/message

# Once an EROFS fsimg reader exists:
fsimg if fixtures/core-4k.erofs
fsimg tf fixtures/core-4k.erofs
fsimg xOf fixtures/core-4k.erofs /etc/message
fsimg Wdf fixtures/core-4k.erofs
```

The symlink fixture requires appropriate `fsimg` policy when extracting and comparing trees; `fsck` extraction options need to be selected for xattrs/links if testing those. If the source directory has a file named `empty`, verify size zero separately; a successful `cmp` on nonempty files alone would miss zero-length inode errors.

### 15.2 Alternative baseline geometry and inode variants

```sh
mkfs.erofs -b 4096 -E force-inode-compact -U clear -T 0 \
  fixtures/core-compact.erofs fixtures/tree
mkfs.erofs -b 4096 -E force-inode-extended -U clear -T 0 \
  fixtures/core-extended.erofs fixtures/tree
mkfs.erofs -b 4096 -E '^inline_data' -U clear -T 0 \
  fixtures/core-no-inline.erofs fixtures/tree

# These modes may be rejected by particular upstream/kernel builds;
# record that as a fixture capability skip, not an fsimg failure.
mkfs.erofs -b 1024 -U clear fixtures/core-1k.erofs fixtures/tree
mkfs.erofs -b 512 -U clear fixtures/core-512.erofs fixtures/tree
```

### 15.3 Compression, tails, fragments, and compressed directories

```sh
# LZ4 default physical clusters; use a tree with repeated multi-block files.
mkfs.erofs -b 4096 -zlz4 fixtures/lz4.erofs fixtures/tree
mkfs.erofs -b 4096 -zlz4hc,12 -C65536 -Efragments,ztailpacking \
  fixtures/lz4-fragments.erofs fixtures/tree
mkfs.erofs -b 4096 -zlz4 -E legacy-compress \
  fixtures/lz4-full-index.erofs fixtures/tree

# Both use their own codec backends; check that mkfs was built with them.
mkfs.erofs -b 4096 -zlzma fixtures/lzma.erofs fixtures/tree
mkfs.erofs -b 4096 -zdeflate fixtures/deflate.erofs fixtures/tree
mkfs.erofs -b 4096 -zzstd fixtures/zstd.erofs fixtures/tree

# Metadata/directory compression: current experimental/feature-dependent.
mkfs.erofs -b 4096 -zlz4 --zD fixtures/compressed-dirs.erofs fixtures/tree
mkfs.erofs -b 4096 -zlz4 -m4096 fixtures/metabox-lz4.erofs fixtures/tree

for img in fixtures/*.erofs; do
  echo "Checking $img"
  fsck.erofs "$img"
  dump.erofs -s "$img" >/dev/null
done
```

A tiny tree may fail to trigger compression at all; **augment it** with at least one 1MiB highly compressible file and one 1MiB deterministic incompressible file, plus compressed subdirectories containing many entries. Verify the *actual emitted feature flags and inode `datalayout`* using `dump.erofs -e --path=/file <image>` before counting a test as compression coverage. Fragments/dedupe often require many small related files. `-E legacy-compress` is obsolete but useful solely to construct old full-index fixtures. `-m4096` and `--zD` are optional newer features: skip with a recorded version/compile reason when unsupported.

### 15.4 Chunk maps, external blob files, 48BIT, large addressing

```sh
# Fixed chunks without separate blob; choose a source with sizeable files.
mkfs.erofs --chunksize=4096 fixtures/chunks-4k.erofs fixtures/tree
mkfs.erofs --chunksize=65536 -E force-chunk-indexes \
  fixtures/chunks-indexes.erofs fixtures/tree

# External blob: mkfs creates/uses the named blob file, and image references it.
mkfs.erofs --chunksize=4096 --blobdev fixtures/payload.blob \
  fixtures/external.erofs fixtures/tree
fsck.erofs --device=fixtures/payload.blob fixtures/external.erofs

# New extension families: large *logical* input data, sparse backing where feasible.
mkfs.erofs -E 48bit fixtures/48bit.erofs fixtures/tree
mkfs.erofs -E dot-omitted fixtures/dot-omitted.erofs fixtures/tree

# A standalone EROFS image from tar (requires a tar writer available to host).
tar -cf fixtures/tree.tar -C fixtures/tree .
mkfs.erofs --tar=f fixtures/tar-full.erofs fixtures/tree.tar

# A metadata-only tar index: non-standalone; original tar is a required blob.
mkfs.erofs --tar=i fixtures/tar-index.erofs fixtures/tree.tar
```

The CLI shapes above reflect current `erofs-utils` manuals, but **the availability of `--chunksize`, `--blobdev`, `-E 48bit` and `--tar=i` is version- and build-dependent**. In the proposed `fsimg` API, external blob files are supplied **explicitly**; probing the primary index must not automatically load `fixtures/payload.blob`. For tar metadata-only images, the original tar (or correct byte-for-byte data source) must be passed through a safe read-only device binding before reading external data; use the tooling's `--device` options where supported.

### 15.5 Focused inspect/read/test utilities

```sh
# Print geometry, features and volume data.
dump.erofs -s fixtures/lz4.erofs
# Print path's extent map and compressor distribution.
dump.erofs -e --path=/etc/message fixtures/lz4.erofs
# Dump one file's logical bytes (binary-safe redirection).
dump.erofs --cat --path=/etc/message fixtures/lz4.erofs > /tmp/erofs-message
# Check image and its referenced file payloads.
fsck.erofs fixtures/lz4.erofs
# Extract for tree/hash comparison; output directory must be clean.
mkdir -p fixtures/output
fsck.erofs --extract=fixtures/output fixtures/lz4.erofs

# Unprivileged Linux/FUSE oracle if built with FUSE:
mkdir -p fixtures/fuse-mnt
erofsfuse fixtures/lz4.erofs fixtures/fuse-mnt
cmp fixtures/tree/etc/message fixtures/fuse-mnt/etc/message
fusermount -u fixtures/fuse-mnt

# Kernel oracle, Linux only, requires permissions and EROFS support:
mkdir -p fixtures/kernel-mnt
sudo mount -t erofs -o loop,ro fixtures/lz4.erofs fixtures/kernel-mnt
cmp fixtures/tree/etc/message fixtures/kernel-mnt/etc/message
sudo umount fixtures/kernel-mnt
```

The `erofsfuse` oracle can be available without kernel EROFS support and may differ in extension coverage and bugs. Kernel mount may refuse an image whose feature requires a kernel newer than the host: record a version-based expected skip rather than treating it as a filesystem mismatch. On macOS and FreeBSD the userspace tools and Go library can provide a portable oracle; `mount -t erofs` is not a portable native test there.

### 15.6 fsimg writer → independent readers (once implemented)

```sh
# Proposed command once erofs creation is registered:
fsimg cWRf fixtures/fsimg-core.erofs -F erofs -C fixtures/tree .
fsimg Wdf fixtures/fsimg-core.erofs
fsck.erofs fixtures/fsimg-core.erofs
dump.erofs -s fixtures/fsimg-core.erofs
mkdir -p fixtures/fsimg-core-out
fsck.erofs --extract=fixtures/fsimg-core-out fixtures/fsimg-core.erofs
cmp fixtures/tree/etc/message fixtures/fsimg-core-out/etc/message

# Reproducibility: two disjoint outputs must match byte-for-byte.
fsimg cRf fixtures/fsimg-r1.erofs -F erofs -C fixtures/tree .
fsimg cRf fixtures/fsimg-r2.erofs -F erofs -C fixtures/tree .
cmp fixtures/fsimg-r1.erofs fixtures/fsimg-r2.erofs
```

`cWRf` runs fsimg's verify-before-publish path if the driver advertises the requisite writer+verify capabilities. All paths used in this example are new files; fsimg correctly refuses to overwrite existing destinations. On every fixture, compare **file hash, file size, mode, ownership, symlink target and type**, not only successful mounts or textual listings.

## 16. Qualification matrix and adversarial tests [P]

### 16.1 Positive matrix — independently generated input

Every case needs a provenance record: `producer, version/tag, exact args, host/OS, image SHA256, expected tree SHA256 / mtree, expected EROFS feature bits, expected layout IDs`. A skipped fixture due to missing codec or newer kernel is **uncovered**, not passing. Read `fsimg` output independently, then compare to both reference reader and direct expected source content.

| Family | Minimum witness | Required checks |
|---|---|---|
| Probe/geometry | 512, 1024, 2048, 4096 block sizes; wrong magic; prefixed/partitioned image | Correct detection boundary, byte offset 1024, volume slicing, read-only open |
| CRC | Enabled, disabled, checksum corrupted, 512B and 4KiB | Correct CRC32C seed, length/zeroing, exact mismatch error |
| Inode versions | Compact, extended, both in one image | Correct sizes, UID/GID, mode, file size, times |
| File sizes | 0, 1, 31, 32, 127, B-1, B, B+1, 2B-1, 2B, >4GiB sparse | Inline-tail block edge and large arithmetic |
| Directory names | 1, 255, non-ASCII, invalid UTF-8, deep dirs, many >one block | Raw bytes, ordering, safe extraction, no casefold |
| Symlinks | Relative, absolute, length1, length1024, non-UTF8 | Byte-accurate target, no following on read |
| Hardlinks | 2+ dirents one NID, directories distinct NIDs | Link count/inode identity, no duplicate decoding bugs |
| POSIX kinds | FIFO, char, block, socket; unknown type entry | Correct read-only recognition and metadata-loss policy |
| Timestamps | Core compact fixed `(epoch,fixed_nsec)`, 48BIT compact per-inode delta, extended mtime, mismatched `MTIME` | Correct semantics, range and `-R` writer |
| Xattrs | Inline only, shared, multiple, ACL, security label, user attr | Namespace names, value bytes, metadata omission report |
| Long prefixes | Prefix in packed inode, standalone, metabox | Reconstructed names, padding, invalid selector behavior |
| Bloom filter | Enabled, disabled, reserved nonzero, false positive | No false negatives; full verification scans actual entries |
| Chunked | Simple 4B map, 8B indexed, multiple sizes, holes | Exact byte address, size tail and known-zero range |
| Devices | 1, 2, explicit and unified IDs, missing blob | Correct device bounds/tag handling, missing-source error |
| Compressed full | LZ4, LZMA, DEFLATE, Zstd; HEAD1/2 | Exact logical bytes, partial first/head, codec dispatch |
| Compressed compact | 2B, 4B, mixed, multiple groups | Bit pack decoding, boundary lookups, EOF |
| Large pclusters | B, 16KiB, 64KiB, 128KiB, near max | Compressed block count/working-set cap |
| Compression tails | ztailpacking, fragments, all-fragments | Correct final physical location and decoded length |
| Dedupe | Exact shared data, similar-but-different | Shared extent legal, exact file contents both sides |
| Packed inode | All-fragments, fragment + ordinary pcluster | Correct special reference and recursion control |
| Encoded extents | `48BIT`, all 4/8/16/32-byte modes if producible | Explicit/implicit offsets, partial reference, byte addressing |
| Metabox | Uncompressed, compressed, xattrs and dirs inside | Alternate metadata domain and bounded recursion |
| Modern layering | tar-full, tar-index, OCI EROFS layer | Distinguish standalone and externally backed payload |
| fsimg layers | raw image, GPT partition, compressed outer layer | Identical logical data no matter the source envelope |
| I/O | tiny read windows, random seek, short-read, pipe input | Correct seek/spool behavior; no out-of-bounds reads |
| Reproducibility | same source with different host enumerate order | `-R` identical output bytes and timestamp/UUID |
| Failure cleanup | allocator and I/O faults during each callback | No leak, stale held pointer, unpublished half image |

### 16.2 Negative/mutation matrix — do not just flip the magic

Start from **at least two independent valid producer images**, mutate one field at a time, and recompute CRC when the mutation tests deeper parsing (otherwise every mutant merely fails the header checksum). Use a Ruby byte-level fixture mutator and an independent field-layout table, not the fsimg implementation parser to generate expected validity. Cover:

- Superblock: short image; half magic; illegal `blkszbits` shift; invalid `dirblkbits`; `sb_extslots` overflow; `checksum` mismatch; unknown incompat vs unknown compat; `meta_blkaddr*B` overflow; `rootnid` outside actual image; `blocks=0`; adversarial `blocks` larger than image; ambiguous 48bit root field; config table larger than volume.
- Inodes: truncated compact/extended body; invalid `i_format` layout5–7; reserved bits; `i_size` near `UINT64_MAX`; invalid `i_mode` type; invalid nsec >= 1e9; impossible xattr count; inline tail crosses metadata block; malformed symlink target; impossible `startblk`/high bits; embedded shared/metabox domain cycles.
- Directories: first `nameoff` zero/not multiple of12/too large; decreasing or overlapping names; filename crosses block; duplicate names; unsorted names; slash, embedded NUL, empty name, >255 bytes; invalid type and type mismatch; NID outside device; dangling child; `..` mismatch; self/parent cycle; dense directory with many blocks.
- Xattrs: short 12-byte header; impossible `h_shared_count`; shared index into EOF; zero/non-4-aligned entry size; name/value length overflow; unknown namespace; long prefix out of range; prefix cycle; invalid prefix `size`; name filter falsely claiming present attr absent; malformed ACL value.
- Chunked: `chunkbits` causing shift overflow; too few index bytes; null address hole, illegal device ID; unified ranges overlapping; extra device not provided; slot beyond primary; `uniaddr + blocks` overflow; chunk physical region crossing blob EOF; final chunk overread.
- Compression: unsupported algorithm mask; bad/missing config; truncated map header; huge `h_idata_size`; invalid `lclusterbits`; NONHEAD delta zero/backward cycle; bad first/last 2B pack; `D0_CBLKCNT` without big pcluster; claimed decoded bytes greater than limit; compressed physical length beyond image; invalid LZ4 backreference; truncated MicroLZMA; raw DEFLATE bitstream invalid; Zstd window demand beyond bound; invalid zero padding; fragment offset >packed file length.
- Experimental: 48bit address high bits overflow, synthetic huge NID, unknown encoded-extent width, count greater than available records, decreasing logical offsets, omitted fields misinterpreted, metadata-in-metabox reference looping to itself; incorrect `SHARED_EA_IN_METABOX` placement.
- Runtime: short reads at every byte boundary, mid-operation source mutation (identity race), zero budget / 16MiB budget, interrupted spool/emit, malformed sparse hole report, repeated open/close, many nested directories, millions of tiny entries (limited by input-size work budgets).

Each mutation should assert: precise error category, no read outside bounded input, no infinite traversal/decompression loop, no unexpected device open, no output published on failed create, and no process crash. Variants invalid *only* under full verification should still be probeable/openable when they do not invalidate the root—test the failure at the correct API boundary.

### 16.3 Suggested concrete fixture patterns

Create deterministic source files with small standalone scripts (Ruby is already fsimg's independent test language):

- One 0-byte file, one 1-byte file, 2 files exactly B bytes, one B-1 and one B+1 to separate inline/flat cases.
- One all-zero 1MiB file; another that alternates 4KiB zero and pseudorandom 4KiB pages; one deterministic 1MiB pseudorandom file; one repeating plain text file. Make expected hashes known and fixed.
- 1000 names sharing a 20-byte prefix but having unique bytewise suffixes; a separate directory with 1-byte single-character names so records fill many blocks; include a filename with high-bit bytes if host permits.
- Ten identical medium files and ten near-identical files; this distinguishes dedupe correctness from digest-collision assumptions and encourages fragment reuse.
- Source files carrying `user.test`, `security.*` (when permitted by host), and POSIX ACL attributes. For reproducibility, prefer an mtree/xattr fixture recipe plus a known-good image checked into fixtures (subject to binary distribution policy) rather than requiring root to set all namespaces.
- Source directory and file timestamps including 0 seconds, >2038, subsecond and different nsec values to force extended inodes.
- Subtree with a hardlink target and multiple aliases (if fixture producer supports hardlinks), and symlinks pointing inside/outside the tree without following them.

### 16.4 Differential test protocol

For every valid image, construct a canonical `expected.jsonl` manifest of `path raw-name encoding / file type / logical size / owner/mode / precise known times / symlink target / SHA256(file payload)` using the original source or a separately authored model. Verify the same from `fsimg`, `erofsfuse` (when available), `fsck.erofs` extraction and `go-erofs`; never rely exclusively on `fsck` in a feature family with a known version-specific bug. For writer images, compute a second manifest by extracting with `fsck.erofs` and compare it against the *source* manifest. For sparse files, compare both zero-range facts and the full logical content hash.

Use Ruby to build minimal hand-authored **uncompressed** fixtures with a few inodes/directories and compute CRC32C independently from fsimg. The point is an independent construction, not a second wrapper around fsimg's writer code. Use byte-offset mutation and a shadow parser to validate EROFS structures in C tests. Separately run AddressSanitizer/UndefinedBehaviorSanitizer and `libFuzzer` on `super`, `inode`, `directory`, `xattr`, `chunkmap`, and `zmap` functions, with memory and work caps set to public minima.

---
## 17. External reader/writer/test tools: exact roles and limitations

All the following are **external tools or references**. `fsimg` should not invoke them at runtime. The commands below are for fixture generation and interoperability checks; do not run privilege-requiring operations in a common C unit test. Pin a particular release/tag/commit for every fixture.

| Tool / project | Write EROFS | Read/list/extract EROFS | Verification use | Important limitations / platform / license |
|---|---|---|---|---|
| [`mkfs.erofs`](https://github.com/erofs/erofs-utils/tree/dev/mkfs) (`erofs-utils`) | **Yes**: native directory, tar, OCI, S3, rebuild/incremental sources; compression, chunks, fragments, new extensions | Not a browsing tool | Primary independent *writer* oracle | Linux/macOS build; some features require optional libs; MIT for tool files in current tree, individual SPDX governs |
| [`fsck.erofs`](https://github.com/erofs/erofs-utils/tree/dev/fsck) | No (not a repair/rewriter) | **Yes**, `--extract`; `--nid`, `--path` | Primary structural + payload **checker** | Always run `--extract` when file payload/decompression matters; the non-extract pass alone is not proof every compressed byte was decoded; no data repair |
| [`dump.erofs`](https://github.com/erofs/erofs-utils/tree/dev/dump) | No | **Yes**: `--ls`, `--cat` on NID/path | Superblock, extents (`-e`), statistics (`-s`, `-S`) | Independent *reporting* oracle but shares `liberofs` with mkfs/fsck; thus not fully independent of them |
| [`erofsfuse`](https://github.com/erofs/erofs-utils/tree/dev/fuse) | No | **Yes** through FUSE mount | Whole-tree POSIX stat/read / random reads without a Linux kernel EROFS mount | FUSE availability/permissions, extension support and directory behavior may vary; shares upstream parsing code |
| [`mount.erofs`](https://github.com/erofs/erofs-utils/tree/dev/mount) | No | Mount helper; file/remote mounts | Test file-backed or remote mount workflows | Different from `erofsfuse`; newer versions support remote sources (S3, OCI) and UBLK/Fanotify depending on configuration/kernel |
| Linux kernel [`fs/erofs`](https://github.com/torvalds/linux/tree/master/fs/erofs) | No, filesystem is immutable | **Yes**: mounted read, mmap, stat, xattr | Most important independent runtime interoperability target | Linux-only, requires filesystem support and possibly root or mount namespace permissions; kernel-dependent features; **GPL kernel source not imported** |
| [`go-erofs`](https://github.com/erofs/go-erofs) | **Yes**: Go `fs.FS` input, merged layers, metadata-only outputs | **Yes**: basic uncompressed image, chunk map, xattrs, external blobs | Independent Go reader *and* writer oracle especially for chunk/external-device/OCI cases | Apache-2.0; **the project's current README explicitly marks compressed-image reading unchecked/unsupported**. Do not use as compression oracle |
| [`rust-fs-erofs`](https://github.com/antimatter-studios/rust-fs-erofs) | **Yes**: uncompressed CLI writer, LZ4/LZMA/DEFLATE library writing (per project) | **Yes**: LZ4/LZMA/DEFLATE/Zstd and numerous advanced layouts | Independent, clean-room, pure-Rust reader/writer with published test/feature matrix | MIT, available on Linux/macOS/Windows; **48-bit and metabox explicitly refused**; distinguish its `mkfs.erofs` command from upstream one |
| [`erofs-rs`](https://github.com/erofs/erofs-rs) | Work in progress | Work in progress | Additional independent parsing/reference tests | MIT; upstream's 2026 GSoC description still identified significant gaps, including xattr, multi-device and 48-bit support. Do not assume feature parity from names of subdirectories |
| [`erofs-image-spec`](https://github.com/erofs/erofs-image-spec) | Transport definition, not a mkfs | Transport specification, not parser | Test OCI/image-layer compatibility and external blobs | Apache-2.0. **Not** the byte-level EROFS inode specification |
| [`simg2img`](https://android.googlesource.com/platform/system/core/+/refs/heads/main/libsparse/) (Android libsparse) | Writes **raw image** from Android sparse container, not EROFS data itself | Unwraps Android sparse transport | Obtain canonical raw EROFS from Android sparse system/vendor images | It handles sparse container envelope; never treat Android sparse magic as EROFS superblock magic; AOSP per-file license must be inspected if importing |
| `file` / libmagic, `xxd`, `od`, `hexdump` | No | **Identification only** | Quick sanity/probe, offset inspection | Not full filesystem readers; `xxd` and raw offsets can confirm magic only |
| `sha256sum`, `shasum -a 256`, `cmp`, `diff`, `stat`, `getfattr` | No | Host-output inspections | Independent content and metadata comparison after extraction | `getfattr` is Linux-oriented; exact platform metadata tools differ |
| `ruby` or `python3` + a deliberately small independent byte parser/mutator | **Yes**, can build primitive hand-authored core fixtures | **Yes**, independent superblock/dirent/NID validation | Mutations, checksum vector tests, exact record assertions | Own implementation must be original, tested against reference producers; cannot count code copied from the EROFS kernel as independent oracle |

### 17.1 Testing tool installation/build notes

On a Linux host, install upstream `erofs-utils` through the distribution package (`erofs-utils` on major distributions) and confirm `mkfs.erofs -V`, `fsck.erofs -V`, `dump.erofs -V`, and `mkfs.erofs --help` **before** issuing any feature-specific test. `erofsfuse` might not be in the packaged binaries, and older distro versions won't know `48bit`, `-m`, `--zD`, or new `-E` options.

To build upstream at a pinned revision, follow [erofs-utils installation documentation](https://github.com/erofs/erofs-utils/blob/dev/docs/INSTALL.md) and its autotools flow. LZ4 support uses `liblz4`; MicroLZMA needs a sufficiently new `liblzma`/XZ Utils; DEFLATE and Zstd use their respective codec libraries; FUSE needs libfuse. Confirm each codec was compiled in by looking at `mkfs.erofs --help` and generating a tiny fixture; a successful `configure` is not evidence that all algorithms are enabled. `erofs-utils` 1.9.4 fixed several fragment/extraction and macOS compression issues, so use it or later when judging a regression; retain older versions as legacy-compatibility tests.

`go-erofs` is a Go library rather than a guaranteed installed `go-erofs` CLI; write a small **separately authored test harness** around its exported `Open`, `Create`, `CopyFrom` and `fs.FS` interfaces. Avoid assigning hypothetical CLI flags to it. Its own [README](https://github.com/erofs/go-erofs/blob/main/README.md) explicitly lists compression reading as not implemented.

The independent Rust project has its own reader CLI **`fs.erofs`**, and its build may install its own program named `mkfs.erofs`; keep separate `PATH` prefixes so the two writers can be named, e.g. `${EROFS_UPSTREAM_BIN}/mkfs.erofs` versus `${EROFS_RUST_BIN}/mkfs.erofs`. Its versioned [feature register](https://github.com/antimatter-studios/rust-fs-erofs/blob/main/docs/features.md) should decide which test family it actually covers, not its general README headline. Crate releases appear as `rust-fs-erofs`; the older name `am-fs-erofs` is superseded. The 0.5.0 crate was published on **19 October 2026**.

On macOS, use upstream `fsck.erofs` and `dump.erofs` (built locally or from packages) as primary read oracles, the pure-Go and pure-Rust projects as secondary oracles, and run the Linux kernel mounted-image gate in a separate Linux CI environment. On FreeBSD, attempt upstream userland compilation and the independent Go/Rust oracles, but record unsupported native mount/FUSE environments as skipped. **Do not require Docker** for any of these suites; ordinary local executables and a Linux virtual machine are adequate.

### 17.2 Recommended mandatory independent checks per feature

| Feature / failure class | Writer/oracle | Second oracle | Independent byte-level assertion |
|---|---|---|---|
| Core uncompressed | Upstream mkfs/fsck | Rust writer/reader or Go writer/reader | Magic, super length, NID math, directory nameoff and physical extent |
| CRC and feature bits | Upstream mkfs/fsck | Linux kernel mount | CRC32C over precise documented *implementation* length; reject unknown incompat |
| Inline tails and symlinks | Upstream mkfs/fsck | Rust reader, kernel | Stored logical bytes at metadata-tail offset, no cross-block overread |
| xattr/prefixes | Upstream mkfs/fsck | Go/Rust reader and kernel `getfattr` | Correct ID/prefix decoding, offsets, no false-negative lookup |
| Chunking + blobdev | Upstream mkfs/fsck `--device` | Go/Rust reader or kernel with `device=` | Physical range and device ordinal; no silent missing-blob reads |
| LZ4/MicroLZMA/DEFLATE/Zstd | Upstream mkfs/fsck + kernel | Rust reader | Map header/index and decoded SHA; Go does **not** qualify this family |
| Big pcluster/compacted indexes | Upstream mkfs/fsck + kernel | Rust reader | Non-head lookback, group index and decoded-length bounds |
| Packed fragments/ztailpacking | Upstream mkfs/fsck + kernel | Rust reader if feature register supports profile | Verify tail extent and packed-inode bounds separately |
| 48-bit encoded extents | Recent upstream mkfs/fsck + recent kernel | Independently authored small field/extent checker | Address widths, record-size format, byte offsets, >32-bit math |
| Metabox/metadata compression | Recent upstream mkfs/fsck + supporting kernel | Independent byte-level meta-domain traversal | Flagged NID domain, no recursion, exact decoded metadata bounds |
| OCI/tar-index | Upstream mkfs + blob sources | Go layer writer/reader where applicable | Self-contained vs externally-backed classification and whiteout semantics |

Never count upstream mkfs, fsck, dump and erofsfuse as four *independent implementations*: they substantially share `liberofs` and may share a decoding bug. Independent Go and Rust projects are valuable specifically because they do not use `liberofs` internally, within their actual advertised feature scopes.

## 18. Primary documentation, existing code and research references

Every URL below is a useful implementation, diagnostic, or format reference; this is an inventory of **links and independently derived factual pointers**, not a license to copy third-party text or code. Licenses here describe the project/file as verified on 6 November 2026 where specified; inspect an exact commit's SPDX tag if reuse is ever proposed.

### 18.1 Formal/reference EROFS on-disk documentation

| Link | What to consult | Provenance / restriction |
|---|---|---|
| [EROFS on-disk format documentation index](https://erofs.docs.kernel.org/en/latest/ondisk/index.html) | Entry point: core records and optional extensions | Upstream public documentation; explain in original words rather than copying its text/figures |
| [Core on-disk structures](https://erofs.docs.kernel.org/en/latest/ondisk/core_ondisk.html) | Magic and geometry, 128B superblock, inode layouts, directory encoding, feature flags, CRC claim | Primary wire-format reference, cross-check implementation discrepancies |
| [Extended attribute format](https://erofs.docs.kernel.org/en/latest/ondisk/xattrs.html) | Inline/shared xattrs, name namespaces, long prefixes, filters | Primary extension reference |
| [Chunk-based file format](https://erofs.docs.kernel.org/en/latest/ondisk/chunked_format.html) | Chunk size/bits, 4B and 8B maps, multi-device | Primary extension reference |
| [EROFS design rationale](https://erofs.docs.kernel.org/en/latest/design.html) | Block alignment, compressed-extent philosophy, deduplication, performance | Background rationale, not substitute for a byte specification |
| [Official build/mount and utility instructions](https://erofs.docs.kernel.org/en/latest/mkfs.html) | `mkfs.erofs` options, compression and simple test commands | Authoritative native test setup |
| [EROFS layer and image merging](https://erofs.docs.kernel.org/en/latest/merging.html) | Layered images and data/metadata separation | For future container/image extensions |
| [Linux kernel EROFS documentation](https://docs.kernel.org/filesystems/erofs.html) | Kernel flags/mount options, new capabilities, file-backed and device-backed behavior | Integration qualification and kernel-version coverage |
| [EROFS project home](https://erofs.docs.kernel.org) | Project news, docs and active development | General entry point |
| [USENIX ATC 2019 EROFS paper](https://www.usenix.org/conference/atc19/presentation/xiang) | Historical design / compression rationale | Research paper; later wire extensions supersede early presentation |
| [EROFS mailing list archives](https://lore.kernel.org/linux-erofs/) | Edge-case discussions, proposals, bug fixes, incompatibilities | Public thread discussions; distinguish proposals from shipped format |
| [Upstream EROFS project repositories](https://github.com/erofs) | Discover maintained utilities, doc repos and integration forks | Project index |

EROFS project documentation is available under a **CC-BY-SA-4.0 documentation license** in the documentation repository; it is *not* treated here as source code to import, and its language or diagrams have not been copied. Published field sizes, constants, addresses, and interoperability rules are restated as independently arranged technical facts.

### 18.2 Canonical userspace tools, on-disk header and implementation

Current project: [`erofs/erofs-utils`](https://github.com/erofs/erofs-utils) ([license summary / `COPYING`](https://github.com/erofs/erofs-utils/blob/dev/COPYING), [release `ChangeLog`](https://github.com/erofs/erofs-utils/blob/dev/ChangeLog)). **Most** files under `lib/` and `include/` now use `GPL-2.0+ OR MIT`; other files are generally MIT unless an individual SPDX says otherwise. This was clarified/updated in **1.9.2, July 2026**; the 1.9.4 changelog is dated **19 August 2026**. These links are for analysis, not copied implementation:

| File/link | Why it matters for fsimg |
|---|---|
| [`include/erofs_fs.h`](https://github.com/erofs/erofs-utils/blob/dev/include/erofs_fs.h) | **Most useful single technical header**. Superblock/compact/extended inode, xattr, chunk, directory, compressed header/index, 48-bit extents and constants. Current SPDX **MIT** |
| [`lib/super.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/super.c) | Version/feature negotiation, superblock CRC coverage, device table, compression config, 48-bit roots, experimental behavior |
| [`lib/namei.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/namei.c) | NID-to-offset, inode version, owner/mode/mtime and file kind decoding, directory lookup |
| [`lib/dir.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/dir.c) | Directory-record iteration, last-name boundary, sorted order, dot-dot validation |
| [`lib/inode.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/inode.c) | Inode allocation/writer, NID assignment, directory build and type handling |
| [`lib/data.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/data.c) | Flat/inline data mapping, chunk block/index maps, missing extent/holes, multi-device routing |
| [`lib/zmap.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/zmap.c) | **Most useful compression-index reference**. Full 8B records, mixed 2B/4B packing, HEAD/NONHEAD traversal, encoded extent mappings, fragments |
| [`lib/decompress.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/decompress.c) | Compression dispatch, per-codec decoding boundaries and shape |
| [`lib/compress.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/compress.c) | Writer compression decisions, physical cluster layout, inline tails, extent encoding |
| [`lib/xattr.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/xattr.c) | Inline/shared name/value decoding, long prefixes, namespace indexes, name filter and writer |
| [`lib/fragments.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/fragments.c) | Packed-inode fragments and writer bookkeeping |
| [`lib/metabox.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/metabox.c) | Experimental compressed metadata domain; crucial for newer variant |
| [`lib/rebuild.c`](https://github.com/erofs/erofs-utils/blob/dev/lib/rebuild.c) | Source image rebuild and layering strategies |
| [`mkfs/`](https://github.com/erofs/erofs-utils/tree/dev/mkfs) and [`mkfs/main.c`](https://github.com/erofs/erofs-utils/blob/dev/mkfs/main.c) | Command line options, generated profiles, fixture creation and writer |
| [`fsck/`](https://github.com/erofs/erofs-utils/tree/dev/fsck) | Image correctness validator and extractor |
| [`dump/`](https://github.com/erofs/erofs-utils/tree/dev/dump) | Inspected superblock, inode and physical map display |
| [`fuse/`](https://github.com/erofs/erofs-utils/tree/dev/fuse) | User-space filesystem read oracle |
| [`mount/`](https://github.com/erofs/erofs-utils/tree/dev/mount) | User-space mount helper and remote backends |
| [`man/mkfs.erofs.1`](https://github.com/erofs/erofs-utils/blob/dev/man/mkfs.erofs.1) | Detailed writer options and extension switches; use in test recipes |
| [`man/fsck.erofs.1`](https://github.com/erofs/erofs-utils/blob/dev/man/fsck.erofs.1) | Verification/extraction flags and limitations |
| [`man/dump.erofs.1`](https://github.com/erofs/erofs-utils/blob/dev/man/dump.erofs.1) | Listing, `--cat`, stats and extent introspection options |
| [`docs/INSTALL.md`](https://github.com/erofs/erofs-utils/blob/dev/docs/INSTALL.md) | Dependencies, optional codecs and reproducible build procedure |

**Source selection hygiene:** some `erofs-utils` code is shared by or adapted from the GPL Linux kernel (the relevant individual file explicitly carries an MIT alternative). Even where the MIT option makes legal reuse possible, follow the user's preference here: `fsimg` receives an independently written parser/writer rather than imported text. A new decoder is easier to audit against fsimg's bounded-session API than an imported global-allocation library.

### 18.3 Linux kernel EROFS code — **GPL external reference only**

| Source | What to validate (do not copy source code) |
|---|---|
| [Linux `fs/erofs` directory](https://github.com/torvalds/linux/tree/master/fs/erofs) | Overall read-only VFS implementation, feature evolution |
| [Linux on-disk `erofs_fs.h`](https://github.com/torvalds/linux/blob/master/fs/erofs/erofs_fs.h) | Current kernel's format constants, structures and version gating |
| [Linux `super.c`](https://github.com/torvalds/linux/blob/master/fs/erofs/super.c) | Magic, CRC32C, root/volume validation and supported features |
| [Linux `inode.c`](https://github.com/torvalds/linux/blob/master/fs/erofs/inode.c) | Inode lookup and decoding, special files, 48-bit details |
| [Linux `namei.c`](https://github.com/torvalds/linux/blob/master/fs/erofs/namei.c) | Directory lookup and binary search boundary conditions |
| [Linux `dir.c`](https://github.com/torvalds/linux/blob/master/fs/erofs/dir.c) | Dirent walking and validity |
| [Linux `data.c`](https://github.com/torvalds/linux/blob/master/fs/erofs/data.c) | Flat, inline and chunk map read paths |
| [Linux `zmap.c`](https://github.com/torvalds/linux/blob/master/fs/erofs/zmap.c) | Compression index interpretation and encoded extents |
| [Linux `decompressor.c`](https://github.com/torvalds/linux/blob/master/fs/erofs/decompressor.c) | Kernel decompression runtime and codec limits |
| [Linux `xattr.c`](https://github.com/torvalds/linux/blob/master/fs/erofs/xattr.c) | Namespace, shared entries, long prefixes, bloom filters |
| [Linux `Kconfig`](https://github.com/torvalds/linux/blob/master/fs/erofs/Kconfig) | Which codec/xattr/kernel features are built in vs omitted in a test kernel |

**Licensing rule:** GPL source is a reference oracle only; the authored fsimg implementation and this spec must not contain copied portions. Kernel operational behavior can still be *compared* to fsimg on independently generated images. The Linux kernel's license does not bar examining public *format facts*, but source-code reuse or a function-by-function translation is outside this project plan.

### 18.4 Independent permissively licensed implementations / sources

| Repository / reference | License / observed support | Most useful link(s) |
|---|---|---|
| **go-erofs** | Apache-2.0; reads and writes uncompressed/chunked/xattr/external-device images; merges OCI-style layers; compression reading **not implemented** according to README | [Repo](https://github.com/erofs/go-erofs), [README](https://github.com/erofs/go-erofs/blob/main/README.md), [LICENSE](https://github.com/erofs/go-erofs/blob/main/LICENSE), [Go package API](https://pkg.go.dev/github.com/erofs/go-erofs) |
| **rust-fs-erofs** (Antimatter Studios) | MIT; independent reader for most shipped compressed/index forms and writer for uncompressed CLI + LZ4/LZMA/DEFLATE library; no 48bit/metabox | [Repo](https://github.com/antimatter-studios/rust-fs-erofs), [README](https://github.com/antimatter-studios/rust-fs-erofs/blob/main/README.md), [feature matrix](https://github.com/antimatter-studios/rust-fs-erofs/blob/main/docs/features.md), [LICENSE](https://github.com/antimatter-studios/rust-fs-erofs/blob/main/LICENSE), [crate](https://docs.rs/crate/rust-fs-erofs/latest) |
| **erofs-rs** (EROFS project) | MIT; Rust rewrite, incomplete according to 2026 community description; treat `mkfs/` as experimental until proven | [Repo](https://github.com/erofs/erofs-rs), [LICENSE](https://github.com/erofs/erofs-rs/blob/master/LICENSE.txt), [erofs-sys](https://github.com/erofs/erofs-rs/tree/master/erofs-sys), [dump](https://github.com/erofs/erofs-rs/tree/master/dump), [fuse](https://github.com/erofs/erofs-rs/tree/master/fuse), [mkfs](https://github.com/erofs/erofs-rs/tree/master/mkfs) |
| **erofs-image-spec** | Apache-2.0, defines OCI layering artifact/media format, **not** the EROFS internal byte layout | [Repo](https://github.com/erofs/erofs-image-spec), [LICENSE](https://github.com/erofs/erofs-image-spec/blob/main/LICENSE) |
| **EROFS OCI container toolkit** | Application integration, license of particular files must be checked separately | [Repo](https://github.com/erofs/erofs-container-toolkit) |
| **EROFS backports** | GPL Linux kernel backports; historical compatibility experiments | [Repo](https://github.com/erofs/erofs-backports) |
| **AOSP erofs-utils mirror/import** | Android build image generation and legacy interoperability; per-file licensing/version review required | [AOSP tree](https://android.googlesource.com/platform/external/erofs-utils/) |
| **AOSP libsparse** | Android sparse-wrapper encode/decode, not EROFS filesystem semantics; inspect exact file license | [AOSP libsparse](https://android.googlesource.com/platform/system/core/+/refs/heads/main/libsparse/) |
| **EROFS public format docs source** | CC-BY-SA-4.0, prose/diagrams **not copied** | [Repo](https://github.com/erofs/docs) |

### 18.5 fsimg code and design interfaces to reuse

These **are already part of fsimg's BSD-2-Clause codebase** and are the appropriate utilities and contracts for an EROFS driver:

| fsimg location | Use in proposed EROFS implementation |
|---|---|
| [`README.md`](https://github.com/stass/fsimg/blob/master/README.md) | Current supported formats, CLI tests, platform constraints, reproducibility examples |
| [`lib/formats/format.h`](https://github.com/stass/fsimg/blob/master/lib/formats/format.h) and [`format.c`](https://github.com/stass/fsimg/blob/master/lib/formats/format.c) | Static format registration, pure bounded probe, optional reader/writer |
| [`lib/read/reader.h`](https://github.com/stass/fsimg/blob/master/lib/read/reader.h) | Reader lifecycle, bounded content domain, `held` extents for decoded payloads, metadata reporting |
| [`lib/write/writer.h`](https://github.com/stass/fsimg/blob/master/lib/write/writer.h) | Plan, validate, emit, free, source cursor and sparse-aware writer |
| [`lib/formats/iso9660/`](https://github.com/stass/fsimg/tree/master/lib/formats/iso9660) | Writer emission, directory indexing, strict validation style |
| [`lib/formats/hfsplus/`](https://github.com/stass/fsimg/tree/master/lib/formats/hfsplus) | Multi-layout file decoding and compressed content pathways |
| [`specs/design-formats.md`](https://github.com/stass/fsimg/blob/master/specs/design-formats.md) | Format capability, identity vs validation, error/reporting convention |
| [`specs/design-driver-contract-tests.md`](https://github.com/stass/fsimg/blob/master/specs/design-driver-contract-tests.md) | Driver-independent C reader/writer/verify contract suite |
| [`specs/design-metadata.md`](https://github.com/stass/fsimg/blob/master/specs/design-metadata.md) | POSIX kinds, `FSIMG_META_EXTRA_DATA` and xattr-loss signposting |
| [`specs/design-holes.md`](https://github.com/stass/fsimg/blob/master/specs/design-holes.md) | Known-zero ranges and safe sparse writer behavior |
| [`specs/design-codecs.md`](https://github.com/stass/fsimg/blob/master/specs/design-codecs.md) | Resource-bounded decoder style and third-party code-import policy |
| [`docs/references.md`](https://github.com/stass/fsimg/blob/master/docs/references.md) | Attribution/provenance approach for source files and generated tables |
| [`test/`](https://github.com/stass/fsimg/tree/master/test) | Format-local mutation and contract tests, independent byte checking |

### 18.6 Suggested citation/fixture provenance file

Add `test/fixtures/erofs/manifest.tsv` with one row per native or independently generated image, **without** copying third-party source into fixtures. Suggested fields: `id`, `sha256`, `bytes`, `producer_git_sha`, `producer_license`, `producer_command`, `input_tree_hash`, `codec_backend`, `blocksize`, `feature_compat`, `feature_incompat`, `layout_kind`, `known_external_devices`, `expected_supported`, `first_qualified_fsimg_commit`. Retain the tool's log (including exact `mkfs.erofs --version`) as an artifact in CI. Pin test oracles by commit, as `dev` is not a stable semantic version.

When archiving external public images, examine source and redistribution terms. Where that is unclear, store just a SHA256, provenance link and command to regenerate. Small custom fixtures built from original Ruby code can be MIT/BSD-licensed within `fsimg`.

## 19. Implementation milestones and release gates [P]

| Gate | Deliverable | Required external pass condition |
|---|---|---|
| **M0: independent miniature model** | Ruby model of superblock, NID, inode/dirent, raw and inline payload; 512/4KiB CRC implementation; self-created minimal image | Upstream `fsck.erofs` validates Ruby-created fixture; Ruby model correctly reads upstream image |
| **M1: detect/info/core reader** | One `fsimg_erofs_format` descriptor, probe/open, `info`, compact+extended, `prepare/replay`, flat and inline, symlinks | Upstream mkfs images and Rust/Go image samples give identical tree/content; `fsimg Wdf` accurately verifies core |
| **M2: core writer** | Deterministic 4KiB raw/uncompressed format, on-disk layouts correct; `cWRf` and `-M`, `-R` | `fsck.erofs --extract` matches original mtree + contents; Linux kernel mounts; repeat builds byte-identical |
| **M3: metadata + noncompressed advanced** | Shared/inline xattrs, name filters, chunk indexes, holes, basic device table, read-only special kinds | Reference parser handles external blobs and xattrs; malformed-image corpus and 16MiB budget suite pass |
| **M4: compression read** | LZ4 → MicroLZMA → DEFLATE → Zstd; full+compact index, mixed 2/4 byte records, fragments, big pclusters, compressed dirs | Independent Rust reader + upstream fsck/kernel decode equal bytes, guarded compressed CPU/memory bounds |
| **M5: compression write** | LZ4 first, then LZMA/DEFLATE/Zstd; deterministic pcluster planning, tail packing, optional dedupe | Upstream reader/`fsck.erofs` extract and kernel mount every advertised profile, byte-level expectation tests pass |
| **M6: new experiments** | `48BIT`, byte-oriented encoded extents, metabox, compressed metadata, longer xattr namespaces, remote images | Version-specific upstream/Linux tests plus original parser/writer reference model, **no default feature enablement without qualification** |

**Key sequencing detail:** M2 can proceed in parallel with M3/M4: most of the initial EROFS writer is a 32B-slot metadata allocator + flat/inline data emitter, not a codec. This fits the existing fsimg scratch-backed prepare/emit architecture, and a valid writer is a valuable second test source for the reader. Avoid waiting for full compression read support before making the first independently-verified writer profile.

The first merge should not claim “full EROFS” support. Advertise **`erofs`** plus a detailed feature profile in volume info, and report unsupported newer images accurately. Expose the accepted block-size/codec/feature matrix in the CLI and API capability surface so test suites know the difference between **format-valid** and **supported by this build**.

### 19.1 Testing and security release checklist

Every release gate requires all of the following: independent raw fixture manifest; positive/negative tests for each added feature; ASan/UBSan on corpus; libFuzzer entrypoint that does not rely on mounts or resource-unbounded decompression; `-l` memory/work budget minima; deterministic error classifications (`EFORMAT`, `ECORRUPT`, `EUNSUPPORTED`); partial-open and cleanup path tests; replay/parallel independent cursor consistency; constrained device source handling; Linux kernel mounted-file comparison; macOS/FreeBSD builds; and a provenance/license review for any new dependency or generated data. Any error from an independent oracle should be triaged as an oracle bug versus fsimg bug with a minimal byte fixture and recorded upstream version.

For core output, verify not merely “kernel mounted it” but all of: root directory present, file lengths, sha256 values, timestamps, mode/UID/GID where available, symlink text, hardlink identity, CRC, and `dump.erofs` extent geometry. For compression, test random byte reads that start or end in the middle of a decoded pcluster and cross pcluster boundaries; whole-file extraction alone can hide a seek-path bug.

## 20. Open questions requiring fixture evidence rather than guesswork

1. **Superblock CRC length in mixed block geometries.** Upstream code's coverage differs from the published guide's simple formula. Generate original 512/1024/2048/4096-byte-block images, flip byte offsets immediately before/after the computed coverage region, and confirm `fsck.erofs` and kernel behavior. Then record a tested CRC vector in an independently authored Ruby fixture, not a transcribed implementation algorithm.
2. **512/1024B block support across real kernels and mount configurations.** Format structures permit a range of block sizes, but runtime page-size and superblock support varies. Qualify builder/reader vs Linux mount separately; don't force a kernel limit into fsimg's format validator unnecessarily.
3. **Unknown compatible bit policy.** `COMPAT` means a conforming reader can ignore unknown compatible extensions while still parsing the underlying layout. But a verifier must avoid certifying semantics it did not check; report understood and ignored bits in volume details.
4. **Plain-inline and compressed-inline placement.** Confirm same-metadata-block boundary and exact tail padding when an inode carries both inline xattrs and a compressed/raw tail; include special packed/metabox address spaces.
5. **Compressed compact-index historical corner cases.** Some older producers emitted zero lookahead values and changed mixed packed-index conventions. Support known valid historical images with documented compatibility exceptions, but reject arbitrary invalid encodings.
6. **`48BIT` superblock root and block alias.** Verify how high block bits, `rootnid_8b`, omitted-dot flag and inode-address high fields interact for *both* small and large images with the incompat bit set. Do not extrapolate a 4B index into a 48-bit variant.
7. **Encoded extent record widths.** 4, 8, 16, 32 byte variants have different addressing and count behavior. Build/verify separate fixtures for every width actually emitted by a pinned upstream producer; flag any non-producible width as unqualified, not implicitly supported.
8. **Metabox self-reference and memory use.** Metadata may be represented by a compressed file; resolve the metabox carrier inode in the primary metadata space and forbid recursive resolution of the same metadata device. Validate its max decompressed run size against fsimg's budget.
9. **Xattr long-prefix placement, shared-in-metabox, image-share xattr.** The same prefix object can reside in a direct pool, packed inode or metabox under different flags. Qualify each combination before enabling writer emission. Remember that the current fsimg public model does not preserve arbitrary xattr *values*.
10. **Extra devices and data-only layers.** External-device IDs, unified addresses and data-only tar index blobs can require adjacent files and source-specific policies. A bare primary image may not be independently readable. Add a safe, explicit device-supply API instead of guessing companion paths and opening arbitrary files.
11. **Directory compression and path traversal.** Reusing the normal decoded-file resolver is attractive, but the reader walker must not spuriously treat a directory as a regular file or use a data content-domain rule that only fits raw file extents.
12. **Writer interoperability versus maximum compression ratio.** The simplest 4KiB, flat/inline writer should ship first. A later compression writer can pursue compressed-size optimizations without compromising deterministic output, exact payload hashes or the strict bounded-memory policy.
13. **`erofs-utils` evolving license.** Current `COPYING` grants many components MIT or GPL/MIT alternatives, but that does not make every older commit or third-party vendor dependency MIT. Each potential imported byte of software would require a separate review; this specification itself includes none.
14. **Global content integrity.** EROFS superblock CRC32C does not authenticate file contents. Dm-verity / fs-verity / signed OCI manifests are *higher-level integrity systems*, not new core EROFS bytes; a `fsimg verify` report should say it validates structural correctness and decodability, **not** cryptographic authenticity.

## 21. Summary of the proposed support contract

The appropriate `fsimg` implementation is a **self-contained, independent C11 EROFS reader/writer** that uses existing `fsimg` bounded I/O, allocation, directory, verification and emission APIs. Core EROFS is small enough to make *read + create + independent verify* an early milestone: the complexity is optional compressed mapping, shared metadata, external data and recent 48-bit/metabox extensions. Keep the basic format clean and reproducible, add advanced features incrementally, and always distinguish unsupported features from corruption. The extensive sources above are for understanding interoperability and constructing independent fixtures; **none of their nonpermissive/proprietary implementation code or prose is incorporated here**.
