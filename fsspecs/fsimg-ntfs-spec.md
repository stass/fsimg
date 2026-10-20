# NTFS 3.x on-disk format and fsimg implementation specification

**Document status:** implementation proposal / research compilation (not a Microsoft normative standard)  
**Research date:** 2026-10-19  
**Target:** NTFS v3.1; read compatibility with v3.0; standalone read/extract and deterministic create-image operations  
**Project:** fsimg  
**Format:** independent description of observable on-disk behavior and implementation requirements

> **Provenance and licensing.** This document is newly written for fsimg. It contains no copied implementation source code, proprietary binary material, or third-party bootloader/upcase-table blobs. Numeric fields, offsets, signatures, and algorithms describe interoperable file-format facts. References to BSD, MIT, Apache, GPL, LGPL, and proprietary implementations are identified as *references or independent testing tools*, not an authorization to copy their code. In particular, **do not copy MIT/Apache code either** if fsimg's project's requirement is literally BSD-only source provenance. Verify file-level licenses before adapting even the historical FreeBSD code. Where publicly available documentation is ambiguous, the text explicitly says so and prescribes differential tests.

## Contents

1. Intended functionality and compliance profile
2. Volume geometry and boot sector
3. The MFT, FILE records, references and torn-write protection
4. Attribute system and exact byte layouts
5. Nonresident runlists and stream reconstruction
6. Directory indexes and name lookup
7. Required metadata files and bootstrap layout
8. Security descriptors, ACLs and alternate namespaces
9. Compression, sparse data, reparse points and advanced features
10. Read/extract algorithm and integrity checks
11. Create/format algorithm and invariants
12. Host-neutral fsimg representation and CLI behavior
13. Testing instruments and reproducible commands
14. Compatibility/test matrix and fuzzing plan
15. Recommended incremental milestones and decisions
16. References (official specifications, BSD and other code, tools)
17. Appendix: offset quick reference and invariants

---

## 1. Intended functionality and compliance profile

### 1.1 Model

An NTFS volume is a random-access sequence of sectors, addressed relative to the **start of the NTFS volume**, not to the beginning of a containing disk. It may appear as a bare filesystem image or at an offset inside an MBR/GPT partition. Its primary object catalog is `$MFT`, whose unnamed data stream contains file record segments. A file or directory is represented by attributes attached to one or more such segments. Its content, filename(s), dates, Windows security, directory index, and optional features are all represented as attribute values.

A *file record number* is an index in the MFT stream, not a disk-sector number. A *file reference* combines that record number with a reuse/sequence counter. Nonresident attributes describe ranges of virtual clusters (VCNs) translated into volume logical clusters (LCNs). Directory names are indexed through `$I30` attributes, with additional index buffers for larger directories.

NTFS is not an archive format: it contains cyclic/self-referential metadata, indexes, bitmaps and optional transactional recovery records. Its physical layout is **not** derivable by concatenating file contents. Building a valid volume requires a whole-image planning pass.

### 1.2 Version and compatibility promise

- **Primary write profile: NTFS 3.1**, ordinary nonbootable data volume, 512-byte logical sectors, 4,096-byte clusters, typically 1,024-byte MFT records and 4,096-byte index buffers. Other geometries should be explicit, separately tested profiles.
- **Primary read profile:** NTFS 3.0 and 3.1, 512/4096-byte logical sectors as actually encountered, geometry read from the volume. Do not assume every MFT record is 1 KiB.
- **Older 1.x/1.2:** recognize/diagnose but do not promise feature-complete support. Older metadata layout and security model differ; accept only once covered by fixtures.
- **Newer Windows:** commonly still reports NTFS 3.1, but operational features (WOF compression, integrity/repair, deduplication, cloud reparse tags, quotas, change journal) may differ.
- **No filesystem-in-place editing** is needed for the initial fsimg interface. All image creation occurs offline; reading is read-only. Refuse dirty/recovery-dependent inputs by default or provide a clearly marked best-effort forensic mode.
- **No bootability promise:** BIOS/UEFI boot files, boot code, BCD, BitLocker and recovery bootstraps are separate systems, not automatically supplied by an NTFS volume writer.

### 1.3 Suggested feature grades

| Grade | Read/extract | Create |
| --- | --- | --- |
| P0 | geometry, MFT records, fixups, attribute lists, resident/nonresident files, root/small directories, basic Unicode names, timestamps | minimal Windows-mountable NTFS volume, mandatory metadata, small directories, resident/nonresident files, valid fixups/bitmaps |
| P1 | fragmented streams, large-directory B+trees, ADS, hard links, sparse files, robust malformed-volume handling | large directories, ADS, hard links, sparse files, deterministic dates and naming |
| P2 | LZNT1 decompression, reparse-point representation, DOS aliases, Windows ACLs, security IDs | selected reparse tags, security descriptor selection, optionally LZNT1 data |
| P3 | WOF/LZX/XPRESS, EAs, USN journal, object IDs, quota/TxF artifacts, advanced corruption recovery | explicitly justified advanced features; no in-place mutation |
| Out of scope | encrypted EFS plaintext without keys, BitLocker ciphertext without unlocking, full `$LogFile` replay, opaque vendor reparse semantics | EFS/BitLocker creation, TxF journaling, bootloader installation, dedup internals |

A new NTFS creator should not claim P0 merely because a third-party library can enumerate its root. **P0 acceptance requires real Windows mounting, file creation, repeated remounts, and clean `chkdsk` results.**

### 1.4 Data, metadata and authority

Distinguish three sources of information:

- **`$STANDARD_INFORMATION`:** authoritative-ish per-file timestamps and DOS/Windows file flags.
- **`$FILE_NAME`:** parent link, filename, namespace, cached dates/sizes/flags. These copies may lag the base-file values.
- **Directory `$I30` index entry:** searchable projection of `$FILE_NAME`, pointing to the file reference. Its payload must agree with the indexed name and reference, although cached fields need not always be byte-for-byte current.

Do not treat `$FILE_NAME` as the byte contents of the file, or `$INDEX_ROOT` as a normal POSIX directory text listing.

---

## 2. Volume geometry and NTFS boot sector

### 2.1 Coordinate system

Let `B` be bytes per logical sector, `S` sectors per cluster, `C = B*S` bytes per cluster, `N` number of sectors in the NTFS volume, and `T = floor(N/S)` clusters. For a volume-local LCN `n`, its byte offset is `n*C`. If the NTFS partition begins at byte `P` in a disk image, the host file offset is `P + n*C`. **Never add the partition offset twice.**

Use 64-bit checked arithmetic. Image bounds may be smaller than the last physical sector declared by a malformed boot sector; reject such volumes unless forensic partial-volume handling is deliberately enabled.

### 2.2 Boot sector fields (offsets from beginning of volume)

All multibyte numbers below are little-endian. These are interoperability facts, not third-party structure declarations.

| Offset | Width | Meaning and validation |
| --- | ---: | --- |
| `0x000` | 3 | x86-compatible jump bytes; do not assume actual executable boot code is present |
| `0x003` | 8 | OEM identifier, ordinarily ASCII `NTFS` plus four spaces |
| `0x00B` | 2 | logical sector size `B`; common 512 or 4096 |
| `0x00D` | 1 | sectors per cluster `S`; power of two, nonzero; validate `C` |
| `0x00E` | 7 | legacy BPB/reserved region; typically zero for NTFS |
| `0x015` | 1 | media descriptor, conventionally `0xF8` for fixed media |
| `0x016` | 2 | legacy reserved field |
| `0x018` | 2 | historical sectors/track geometry; not reliable for addressing |
| `0x01A` | 2 | historical head count; not reliable for addressing |
| `0x01C` | 4 | hidden-sector count (partitions may populate) |
| `0x020` | 4 | NTFS-reserved BPB extension |
| `0x024` | 4 | NTFS-reserved BPB extension |
| `0x028` | 8 | total logical sectors in filesystem volume `N` |
| `0x030` | 8 | starting LCN of `$MFT` |
| `0x038` | 8 | starting LCN of `$MFTMirr` |
| `0x040` | 1 | signed record-size code for MFT records |
| `0x041` | 3 | reserved |
| `0x044` | 1 | signed size code for directory index buffers |
| `0x045` | 3 | reserved |
| `0x048` | 8 | NTFS volume serial number (not a Windows volume GUID) |
| `0x050` | 4 | optional NTFS boot checksum/reserved in known profiles; **do not make it a universal acceptance gate** |
| `0x054` onward | variable | bootstrap/unused region; copying proprietary boot code is forbidden by this project policy |
| `0x1FE` | 2 | final boot-sector signature `0x55,0xAA` for 512-byte-format boot records |

If a size code `q` interpreted as signed 8-bit is **positive**, record bytes = `q*C`. If it is **negative**, record bytes = `2^(-q)`; zero is invalid. Example: code `0xF6` means `-10`, or 1,024 bytes. A code `0xF4` means `-12`, or 4,096 bytes. Validate powers, capacities and physical ranges; do not silently guess. Index and MFT record sizes may use different codes.

An NTFS volume conventionally includes a **backup boot sector at its final logical sector**, at offset `(N-1)*B`. The `$Boot` metadata file also maps the boot-region bytes. Keep these related structures consistent, but do not confuse the last-sector backup with the first sectors of the metadata file.

The legacy `0x55AA` occupies offsets `510..511`; on 4Kn media, the complete sector is larger. Implement sector-size-aware parsing and test 4Kn layout independently rather than assuming all historical 512-byte positions generalize unchanged.

### 2.3 Geometry rules for fsimg

- Default write `B=512`, `C=4096`, 1,024-byte MFT records, 4,096-byte index buffers, NTFS version 3.1.
- Read sizes from the boot sector and enforce `record_size >= 512`, `record_size <= practical_limit`, integer relation with the low-level protection granularity, and image bounds. A MFT record can be smaller than a cluster or span multiple clusters.
- Do not use old CHS values to calculate offsets. Keep volume offset, sector offset and cluster offset as separate named types.
- A sector or cluster outside `[0,N)` or `[0,T)` is invalid; use checked multiplication before read.
- For creation, account for the backup sector, `$Boot` extents, MFT/MFT mirror, system files, potential MFT growth reserve and all data clusters before synthesizing `$Bitmap`.
- Prefer deterministic serial numbers from a supplied seed, but ensure the serial is nonzero and distinguishable across independently created images by default.

### 2.4 Recognition procedure

1. Check minimum length and seekability.
2. Read boot sector at supplied volume offset; confirm OEM marker and plausible jump/signature.
3. Decode geometry; verify cluster size, record/index size, sector count, MFT and mirror LCNs.
4. Try reading FILE record 0 through the initial MFT LCN; validate its update-sequence protection; read `$MFT` runs.
5. If the primary bootstrap is damaged, allow a *diagnostic read-only recovery mode* attempting MFT-mirror and backup-sector recovery; do not reinterpret random data silently.
6. Check `$Volume:$VOLUME_INFORMATION` for version/state after MFT access succeeds.

References: [NTFS `$Boot`](https://flatcap.github.io/linux-ntfs/ntfs/files/boot.html), [Microsoft MFT notes](https://learn.microsoft.com/en-us/windows/win32/devnotes/master-file-table), [Windows FSCTL_GET_NTFS_VOLUME_DATA](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_ntfs_volume_data).

---

## 3. Master File Table, FILE records and update sequence arrays

### 3.1 MFT organization

`$MFT` is the system file at MFT record **0**. Its unnamed `$DATA` stream is a byte array of fixed-size FILE records. If its stream runlist is fragmented, logical MFT record `r` lies at logical stream byte offset `r * mft_record_size`, not necessarily in the first contiguous extent. MFT record 0 has to be bootstrapped from the MFT LCN in the boot sector, then the parsed `$MFT` mapping becomes authoritative.

MFT record **1** describes `$MFTMirr`. The mirror normally contains at least the first four MFT file records; for larger clusters, actual mirrored extent may cover an entire cluster's worth of records. Query/compare the mirror's `$DATA` length instead of assuming it is exactly 4 KiB. The MFT's unnamed `$BITMAP` indicates MFT record allocation, distinct from `$Bitmap`'s cluster allocation.

### 3.2 FILE record header (record-relative offsets)

| Offset | Width | Field |
| --- | ---: | --- |
| `0x00` | 4 | ASCII `FILE` signature; corrupt records sometimes have `BAAD` |
| `0x04` | 2 | update-sequence array (USA) offset |
| `0x06` | 2 | USA element count (16-bit elements, including sequence token) |
| `0x08` | 8 | log sequence number (LSN) associated with the record |
| `0x10` | 2 | record reuse/sequence number |
| `0x12` | 2 | count of hard-link names (interpret cautiously with aliases) |
| `0x14` | 2 | offset of first attribute |
| `0x16` | 2 | flags: bit 0 in-use, bit 1 directory; other flags may identify special records |
| `0x18` | 4 | used bytes within record |
| `0x1C` | 4 | allocated/capacity bytes within record |
| `0x20` | 8 | base-file reference: zero for base records; nonzero for attribute extension records |
| `0x28` | 2 | next attribute instance identifier |
| `0x2A` | 2 | padding/variant-specific |
| `0x2C` | 4 | record number in NTFS 3.1-style header variants |
| subsequent, often `0x30` | variable | update-sequence token and replacement words; first attribute offset is **not** fixed |

Attributes are at `record + first_attribute_offset`, ordered by their type in conventional well-formed records, with a **4-byte terminator `0xFFFFFFFF`**. Attribute instance numbers are local to the containing record. Each attribute record has its own length; attribute-chain traversal must enforce strictly positive progress and record bounds.

### 3.3 64-bit file references

A common NTFS file reference encodes an MFT record number in the **low 48 bits** and a sequence counter in the **high 16 bits**. Thus:

- `record_number = reference & ((1 << 48) - 1)`
- `sequence = reference >> 48`

Before dereferencing an index/parent/attribute-list reference, check `record_number * record_size` for overflow and range. If sequence is nonzero, validate it against the referenced record's sequence counter. An old reference to a reused MFT slot is stale. Base-file and extension-record links must be mutually consistent.

### 3.4 Multi-sector transfer protection (USA / fixups)

Certain NTFS records (`FILE`, `INDX`, `$LogFile` restart/record pages) use an *update-sequence array* to detect incomplete sector writes. An on-disk protected record contains:

1. The offset and count of the USA in its header.
2. A 16-bit **update-sequence number** at the beginning of the USA.
3. One saved 16-bit original trailer for every protected sector in the record.
4. The sequence number substituted into the two trailing bytes of each protected sector in the stored disk image.

**Read:** validate the entire array fits inside the record, check the required number of trailers, compare each stored trailer to the sequence number, then restore the original trailer from the corresponding saved word into a *working copy* of the record. Do not modify the raw read buffer used for byte-for-byte forensic logging.

**Write:** serialize the logical record in memory, retain each protected sector's original final two bytes inside the USA, select a nonzero nonreserved sequence token, overwrite the sector trailers with that token, then write the protected record atomically at the fsimg output layer. Do not overwrite the USA itself with a sector trailer.

In conventional NTFS structures the USA trailer stride is **512 bytes**. A recent kernel bug report specifically reinforces validating the *full* USA, not `count-1` elements, and ensuring the array ends before the final two bytes of the first 512-byte sector. For 4Kn media, do not assume that the physical sector size changes this format-level stride; confirm through real fixtures.

For a `R`-byte record under a 512-byte fixup stride, the expected count is `1 + R/512`. Independently check USA offset, full array end, `first_attribute_offset`, and non-overlap. A corrupt fixup is an integrity failure; never silently repair it during normal extraction. A fuzz target should specifically exercise an array that is one word too long and offsets adjacent to record boundaries.

References: [Microsoft MULTI_SECTOR_HEADER](https://learn.microsoft.com/en-us/windows/win32/devnotes/multi-sector-header), [FILE_RECORD_SEGMENT_HEADER](https://learn.microsoft.com/en-us/windows/win32/devnotes/file-record-segment-header), [2026 USA bounds issue](https://lists.openwall.net/linux-kernel/2026/05/08/1171).

### 3.5 Extension MFT records and `$ATTRIBUTE_LIST`

A file may consume several MFT segments. The first is its **base** segment; each extension segment has a base reference pointing back to it. The base segment may have an `$ATTRIBUTE_LIST` (resident or nonresident) which enumerates the file's attributes and where to find them. A single nonresident stream may be split into multiple attribute-record segments representing separate VCN ranges.

For each attribute-list entry (offset from the entry start):

| Offset | Width | Meaning |
| --- | ---: | --- |
| `0x00` | 4 | attribute type |
| `0x04` | 2 | total entry size, aligned to 8 bytes |
| `0x06` | 1 | attribute name length (UTF-16 code units) |
| `0x07` | 1 | name offset within entry |
| `0x08` | 8 | starting/lowest VCN of described attribute extent |
| `0x10` | 8 | file reference of containing MFT segment |
| `0x18` | 2 | attribute instance ID in that segment |
| `0x1A` onward | variable | optional UTF-16LE name |

Entries are conventionally ordered by type, name and lowest VCN. Identify a stream by `(base_file, attribute_type, attribute_name)`, not merely by record number or first matching attribute. Validate type/instance/name against the referenced attribute; detect cycles, duplicate extents, gaps, overlapping VCN ranges and limits on recursion/number of records. `lowest_vcn` may be greater than zero for the same logical stream, but file size fields in continuation segments are not all authoritative.

Do not build extension-record support into the initial writer if not needed. The reader should support it relatively early, because real Windows volumes contain it even when small fsimg-created files do not.

---

## 4. NTFS attributes: common and variant headers

### 4.1 Attribute type registry

| Type | Name | Role / typical form |
| ---: | --- | --- |
| `0x10` | `$STANDARD_INFORMATION` | resident timestamps, flags, security ID |
| `0x20` | `$ATTRIBUTE_LIST` | links attribute extents stored in other MFT records |
| `0x30` | `$FILE_NAME` | resident parent ref, cached attributes, UTF-16 name |
| `0x40` | `$OBJECT_ID` | file object ID (v3.x; legacy volumes used other interpretations) |
| `0x50` | `$SECURITY_DESCRIPTOR` | individual self-relative security descriptor, mostly older/optional |
| `0x60` | `$VOLUME_NAME` | volume label in `$Volume` |
| `0x70` | `$VOLUME_INFORMATION` | volume version and flags in `$Volume` |
| `0x80` | `$DATA` | unnamed file body, named alternate stream, metadata file body |
| `0x90` | `$INDEX_ROOT` | resident root of filename/other index |
| `0xA0` | `$INDEX_ALLOCATION` | nonresident INDX buffers |
| `0xB0` | `$BITMAP` | cluster/index/MFT allocation bitmap, meaning depends on owner |
| `0xC0` | `$REPARSE_POINT` | reparse tag and payload |
| `0xD0` | `$EA_INFORMATION` | extended-attribute bookkeeping |
| `0xE0` | `$EA` | NT-style extended attributes |
| `0xF0` | `$PROPERTY_SET` | historical/rare; version-specific |
| `0x100` | `$LOGGED_UTILITY_STREAM` | feature-specific logged stream, e.g. EFS-related data |
| `0xFFFFFFFF` | terminator | end of attribute sequence; **not a normal attribute header** |

A robust reader must **skip unknown types safely** by their validated record lengths, preserve opaque bytes only if the extraction target can represent them, and reject an attribute whose length cannot be validated. Always separate *attribute name* from *filename*.

### 4.2 Common header (attribute-relative offsets)

| Offset | Width | Meaning |
| --- | ---: | --- |
| `0x00` | 4 | type code |
| `0x04` | 4 | total attribute record length, 8-byte aligned |
| `0x08` | 1 | residency: 0 resident, 1 nonresident |
| `0x09` | 1 | attribute-name length in UTF-16 code units |
| `0x0A` | 2 | attribute-name offset from this header |
| `0x0C` | 2 | flags; low bits compression algorithm, `0x4000` encrypted, `0x8000` sparse |
| `0x0E` | 2 | attribute instance ID |

Names are **not NUL terminated**; resolve their offsets/lengths entirely within the attribute's validated byte range. Multiple `$DATA` attributes can be present if they have distinct names; one unnamed data stream is the ordinary file body.

### 4.3 Resident header (after the common 16 bytes)

| Offset | Width | Meaning |
| --- | ---: | --- |
| `0x10` | 4 | content byte length |
| `0x14` | 2 | content offset within attribute record |
| `0x16` | 1 | indexing-related resident flag |
| `0x17` | 1 | padding/reserved |

Resident bytes appear within the same MFT record; small file bodies can fit here. Attribute names may precede content; compute offsets instead of assuming value begins at `0x18`. Bound-check both content and name. With 1-KiB MFT records the practical resident payload is much smaller than 1 KiB once all headers, names and fixups are included.

### 4.4 Nonresident header

| Offset | Width | Meaning |
| --- | ---: | --- |
| `0x10` | 8 | lowest VCN covered by this attribute-record segment |
| `0x18` | 8 | highest VCN covered, normally inclusive |
| `0x20` | 2 | runlist/mapping-pairs offset within attribute |
| `0x22` | 2 | compression-unit exponent / variant-specific flags |
| `0x24` | 4 | reserved |
| `0x28` | 8 | allocated byte size of stream (first segment) |
| `0x30` | 8 | logical stream length / data size (first segment) |
| `0x38` | 8 | initialized byte count (first segment) |
| `0x40` | conditional | on compressed/sparse variants, a further 8-byte storage-size field may occur before the name/runlist |
| subsequent | variable | optional UTF-16 attribute name and mapping-pairs array |

**Caution:** Microsoft’s developer-note structure is a simplified view and some versions/attribute flags distinguish the extra compressed-size field. A parser must select the layout from the attribute flags and verified offsets; a writer should avoid compressed/sparse `0x40` header variants until specifically validated against independent readers and Windows.

The first extent (`lowest_vcn=0`) is the source for overall stream size values; for continuation attributes, their corresponding size slots may be undefined/nonauthoritative. Require `0 <= initialized_bytes <= logical_bytes`, with all reads from uninitialized tail logically zero-filled, not leaked from allocated media.

For uncompressed non-sparse content, allocated length is normally the logical size rounded to a whole number of clusters. For compressed/sparse streams, virtual run length, actual allocated clusters, compressed byte count, logical length, and initialized length are **different concepts**; do not collapse them into a single size.

### 4.5 `$STANDARD_INFORMATION` contents

The main 48-byte form has four 64-bit timestamps at offsets `0x00`, `0x08`, `0x10`, `0x18` (created, content-modified, MFT-record-changed, accessed), then four 32-bit fields at `0x20`, `0x24`, `0x28`, `0x2C` (Windows file attribute flags, version counters/class information). The 72-byte extended v3.x form adds at `0x30` 32-bit owner ID, `0x34` 32-bit **security ID**, `0x38` 64-bit quota charge and `0x40` 64-bit USN.

NTFS timestamps are unsigned/signed-compatible 64-bit counts of 100-nanosecond intervals since **1601-01-01 UTC**. Unix time conversion needs a known epoch difference and checked signed operations; preserve native resolution in fsimg metadata even if the host cannot represent it. The security ID is an integer lookup into `$Secure`, **not** a Windows SID or a POSIX UID.

Common Windows file flag values: read-only `0x1`, hidden `0x2`, system `0x4`, archive `0x20`, device `0x40`, normal `0x80`, temporary `0x100`, sparse `0x200`, reparse `0x400`, compressed `0x800`, offline `0x1000`, not content indexed `0x2000`, encrypted `0x4000`; additional Windows-era flags exist. Directory indicator in `$FILE_NAME` caches may use `0x10000000`, not to be confused with MFT-record bit 1.

### 4.6 `$FILE_NAME` contents

Each hard link/name has a resident `$FILE_NAME` value whose offsets are **relative to the attribute content**:

| Offset | Width | Meaning |
| --- | ---: | --- |
| `0x00` | 8 | parent directory file reference |
| `0x08`, `0x10`, `0x18`, `0x20` | 8 each | cached creation, modification, record change, access times |
| `0x28` | 8 | cached allocated size |
| `0x30` | 8 | cached logical size |
| `0x38` | 4 | cached file flags |
| `0x3C` | 4 | EA/reparse-related payload |
| `0x40` | 1 | name length `n` in UTF-16 code units |
| `0x41` | 1 | namespace: 0 POSIX, 1 Win32, 2 DOS 8.3, 3 combined Win32/DOS |
| `0x42` | `2*n` | filename UTF-16LE bytes |

Maximum filename length is generally 255 UTF-16 units (not 255 UTF-8 bytes). A file can have multiple `$FILE_NAME`s including hard links and short DOS aliases. A directory index contains copies of these name values. Do not make a separate extracted file for a DOS alias unless requested; prefer preserving link identity.

### 4.7 `$VOLUME_NAME` and `$VOLUME_INFORMATION`

`$VOLUME_NAME` holds UTF-16LE label characters without a mandatory trailing zero. `$VOLUME_INFORMATION` conventionally has eight reserved bytes, major/minor NTFS version bytes at offsets `0x08/0x09`, and 16-bit flags at `0x0A` (dirty bit `0x0001`, plus other state flags). Do not casually clear a dirty flag on an image requiring journal replay. Use 3.1 as a formatter target, then validate with `fsutil fsinfo` and independent readers.

### 4.8 `$AttrDef` entries

The `$AttrDef` unnamed data stream describes the attribute vocabulary. Known NTFS v3.x entries are 160 bytes each:

| Offset | Width | Meaning |
| --- | ---: | --- |
| `0x00` | 128 | UTF-16LE attribute label, fixed-width storage |
| `0x80` | 4 | numeric type |
| `0x84` | 4 | display rule |
| `0x88` | 4 | collation rule |
| `0x8C` | 4 | attribute-definition flags |
| `0x90` | 8 | minimum content size |
| `0x98` | 8 | maximum content size |

Don't infer every field is semantically active; retain known v3.x values and verify the generated file in `ntfsinfo`, `fsntfsinfo`, Windows or BSD readers. Do not copy a binary `$AttrDef` blob from another license source into the spec.

References: [Microsoft attribute header](https://learn.microsoft.com/en-us/windows/win32/devnotes/attribute-record-header), [STANDARD_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/devnotes/standard-information), [FILE_NAME](https://learn.microsoft.com/en-us/windows/win32/devnotes/file-name), [attribute layout notes](https://flatcap.github.io/linux-ntfs/ntfs/concepts/attribute_header.html), [`$AttrDef`](https://flatcap.github.io/linux-ntfs/ntfs/files/attrdef.html).

---

## 5. Mapping pairs, VCNs, LCNs, resident/nonresident stream reads

### 5.1 Runlist representation

A nonresident attribute maps its logical VCN interval to contiguous physical cluster extents. One mapping-pairs element starts with a control byte `h`:

- low nibble `a = h & 0x0F`: number of bytes storing an **unsigned** cluster-count;
- high nibble `b = h >> 4`: number of bytes storing a **signed** relative LCN delta;
- following `a` bytes: little-endian run length, strictly positive;
- following `b` bytes: little-endian signed two's-complement delta, **sign-extended before addition**;
- `h=0` ends the runlist.

The start LCN is calculated by adding the new delta to the *previous physical run's* start LCN. A run with `b=0` is a **sparse hole** with no allocated LCN; it advances VCN but must not alter the prior physical base LCN. The exact interpretation of the control byte is more reliable than the loose language sometimes found in vendor notes that equates an LCN value of zero with a hole. Physical LCN 0 is not intrinsically a hole.

For each run, track `vcn_start`, `vcn_count`, optional `lcn_start`. The stream's first segment begins at `lowest_vcn`. Additional segments continue with higher starting VCNs. They must reconstruct one nonoverlapping logical stream. A final terminator is mandatory inside the attribute record boundary.

### 5.2 Tiny independently formulated example

A made-up stream with first run 3 clusters starting at LCN 100, a two-cluster hole, and a 4-cluster physical run starting at LCN 92 can be described by the hex bytes:

```text
11 03 64   01 02   11 04 F8   00
```

Interpretation: first run length `3`, delta `+100`; sparse interval length `2`; next length `4`, delta `-8` relative to last physical start, yielding LCN `92`; end marker. The resulting VCNs `0..2` map to LCN `100..102`, VCNs `3..4` are zero-filled holes, and VCNs `5..8` map to `92..95`. All counts and positions must still fit the declared stream and volume bounds. This example is freshly constructed for this document.

### 5.3 Reader invariants

For every run check:

- control byte has reasonable lengths (each nibble at most 8 for 64-bit values), lengths stay inside encoded attribute, cluster count > 0;
- adding signed delta does not overflow or yield a negative physical LCN;
- LCN + count remains within volume cluster count, absent forbidden overlap with reserved metadata if validating own output;
- advancing VCN does not overflow and reconstructed coverage agrees with attribute's declared VCN range;
- direct access to raw bytes never exceeds declared volume length or provided image file;
- sparse data returns logical zero bytes; initialized-length tail zero-fills irrespective of physical allocation;
- partial reads crossing run or cluster boundaries behave correctly.

For compressed attributes, the virtual runlist also represents per-compression-unit layout and zero-padding clusters. You cannot treat every sparse run as independent zero bytes until you determine whether the run lies inside a compressed unit.

### 5.4 Writer mapping-pairs policy

Minimize physical extents to keep the attribute mapping list small. Represent each run length and signed LCN delta with the minimum whole-byte width that round-trips correctly. End with zero control byte and pad the enclosing attribute to 8-byte alignment. Do **not** rely on a high-level allocator to choose contiguous placements after serializing the MFT; allocation must be finalized first.

If the resulting mapping list will not fit in a single MFT record, either (a) generate extension records with `$ATTRIBUTE_LIST` correctly, or (b) fail with a clear `EFBIG`/unsupported-layout error. For an initial offline image writer, selecting contiguous extents and a bounded amount of MFT space is simpler and safer than implementing arbitrary attribute-list generation.

### 5.5 Alternate data streams

An unnamed `$DATA` is `file`'s main content. A named `$DATA` (e.g. `file:metadata`) is a separate byte stream belonging to the **same MFT file**. Read/extraction API must enumerate streams by attribute name and avoid overwriting main content. An empty named stream is distinguishable from a nonexistent stream. Windows path syntax may display stream types such as `:$DATA`; that's API syntax, not part of the `$DATA` attribute's stored name.

For portable extraction, expose ADS as first-class metadata/stream handles. If the target is a directory tree without named-stream support, default to a manifest/sidecar representation instead of silently dropping data or inventing colon-bearing filenames (ambiguous and unsafe on Windows).

References: [Data-runs walkthrough](https://flatcap.github.io/linux-ntfs/ntfs/concepts/data_runs.html), [Microsoft mapping-pairs explanation](https://learn.microsoft.com/en-us/windows/win32/devnotes/attribute-record-header), [Sleuth Kit NTFS stream examples](https://wiki.sleuthkit.org/NTFS-Implementation-Notes/).

---
## 6. Directory indexes and name lookup

NTFS directories are not flat linear record lists. They index `$FILE_NAME` keys using a sorted, potentially multi-level B+tree-like structure named **`$I30`**. A file's name resides in its own MFT record; the parent directory's index contains a searchable copy of the filename attribute and a reference to that record. The copy and original can differ in cached timestamps; the key and reference must still identify the correct entry.

### 6.1 `$INDEX_ROOT` (attribute type `0x90`)

`$INDEX_ROOT` is **resident**. Offsets below are relative to the beginning of its *attribute value*, not its MFT attribute header:

| Offset | Width | Definition |
| --- | ---: | --- |
| `0x00` | 4 | indexed attribute type (`0x30` for `$FILE_NAME`) |
| `0x04` | 4 | collation rule (`1` for filename ordering) |
| `0x08` | 4 | index buffer size, in bytes (frequently 4096) |
| `0x0C` | 1 | size encoding for index buffer (clusters or signed/log-derived representation) |
| `0x0D` | 3 | reserved |
| `0x10` | 16 | common index-node header |
| following | variable | zero or more index entries, including mandatory end-marker entry |

Index buffer length must independently agree with the boot-sector index-record size. Interpret the cluster/count field with the same care as the boot size encoding, particularly when index-buffer size is less than a cluster. A parser must not use `0x20` as a universal first-entry offset; use the common node header's `entries_offset`.

### 6.2 Common 16-byte index-node header

The node header begins at `INDEX_ROOT_value+0x10`, or at `INDX_buffer+0x18`. Its own offsets are:

| Offset | Width | Definition |
| --- | ---: | --- |
| `0x00` | 4 | byte displacement from *start of this header* to first index entry |
| `0x04` | 4 | **used** end offset relative to this header, including node header |
| `0x08` | 4 | allocated end/capacity relative to this header |
| `0x0C` | 1 | flags: bit 0 indicates child/subnode pointers |
| `0x0D` | 3 | reserved |

Important: used and capacity offsets include the common index header portion, not merely the sum of entry payload bytes. Require `0x10 <= entries_offset <= used <= allocated`, with all offsets inside the parent attribute value or protected index block. For a small directory, the root commonly contains all entries plus a terminal entry. There need not be an `$INDEX_ALLOCATION`.

### 6.3 Directory index entries

Directory-entry relative layout (the 16-byte fixed header is followed by a variable `$FILE_NAME` key):

| Offset | Width | Definition |
| --- | ---: | --- |
| `0x00` | 8 | file reference for a real directory entry; not a meaningful target for a terminal entry |
| `0x08` | 2 | full entry length, multiple of 8 |
| `0x0A` | 2 | key length in bytes |
| `0x0C` | 2 | flags: `0x0001` child exists, `0x0002` last/terminal entry |
| `0x0E` | 2 | reserved |
| `0x10` | variable | directory key: a serialized `$FILE_NAME` *content value* |
| end−8 | conditional | 64-bit child-node VCN when `0x0001` set |

The terminal entry **is required** and has no ordinary name key. A terminal entry can still contain a child pointer: it represents the rightmost subtree. For internal-node entries with children, the VCN is located at the **end of the entry**, not directly after the key without alignment. Bound-check `entry_length`, key length, optional 8-byte pointer and 8-byte alignment; require monotonically increasing entry pointers and a reachable terminator. A child pointer in an index with no allocated buffers is corruption.

The key starts with a parent reference, cached dates/sizes and UTF-16LE name as described in §4.6. For extracting names, combine `(parent-file-reference, actual-file-reference, key namespace, UTF-16 name)` carefully; do not confuse index entries with independent hard links or data streams.

### 6.4 `$INDEX_ALLOCATION` and `INDX` record buffers

Once an index grows beyond its resident root, the same named index gains:

- a nonresident **`$INDEX_ALLOCATION`** attribute named `$I30`, whose stream holds fixed-size index buffers; and
- a **`$BITMAP`** attribute named `$I30`, indicating which index buffers are allocated.

Each index buffer contains the following physical layout:

| Offset | Width | Definition |
| --- | ---: | --- |
| `0x00` | 4 | signature ASCII `INDX` |
| `0x04` | 2 | update-sequence array offset |
| `0x06` | 2 | update-sequence array element count |
| `0x08` | 8 | transaction/log sequence number |
| `0x10` | 8 | this index-buffer virtual address (VCN, with encoding dependent on index-buffer/cluster relation) |
| `0x18` | 16 | common index-node header |
| following | variable | index entries, children and fixup array at advertised locations |

Apply sector-trailer fixups **before** interpreting entries. Map a child VCN into `$INDEX_ALLOCATION` with care: when index buffer size is at least the cluster size, VCNs usually have cluster units; when the buffer is smaller than a cluster, the relevant VCN scheme may use sector units. **Do not hardcode `child_vcn * index_buffer_size` for all geometries.** Implement geometry-specific conversion and test images with (a) 4-KiB cluster/4-KiB index and (b) 4-KiB cluster/1-KiB index from independent producers.

The index bitmap uses ordinary bit numbering (least-significant bit first within each byte); its bits refer to index-buffer slots rather than arbitrary filesystem clusters. Require every followed child pointer to address an allocated slot and a correctly signed/fixed-up `INDX` buffer. Reject cycles and pointers to a parent or itself.

### 6.5 Filename comparison and collation

Directory key ordering is **not** locale-sensitive UTF-8 sorting. A normal NTFS volume includes `$UpCase`, a table indexed by 16-bit UTF-16 code unit (usually 65,536 little-endian `uint16` entries, 128 KiB). For NTFS case-insensitive comparison, uppercase code units using that *volume's* table; preserve original case for display. NTFS indexes support names in multiple namespaces; DOS aliases require matching according to their namespace. Tie-breaking/namespace-specific ordering must be validated against Windows-generated directory fixtures.

Requirements:

1. Implement comparison on UTF-16 **code units**, not Unicode scalar-value casefolding; NTFS does not do full Unicode normalization or full casefold expansion like a modern text library.
2. Use **on-volume `$UpCase` when reading**. A host Unicode version may disagree with the table used when the volume was formatted.
3. For creation, produce a deterministic, fully specified 65,536-entry uppercase mapping that has been independently verified for the chosen compatibility profile. Do **not** copy a table from GPL code, a proprietary volume or another non-BSD source into fsimg. An acceptable development path is an independently written generator from openly published Unicode case mappings, plus Windows validation of the *resulting table*. **Important compatibility trap:** choosing the newest available Unicode release does not necessarily reproduce the uppercase map used by a given Windows release. Even `mkntfs` documents `chkdsk` correcting an otherwise functional volume's uppercase file when it disagrees with that Windows version. Treat the target Windows-version mapping as an explicit compatibility profile and require a clean `chkdsk` for it. Record Unicode mapping/version decisions in fixtures ([`mkntfs` Known Issues](https://manpages.debian.org/trixie/ntfs-3g/mkntfs.8.en.html)).
4. Reject duplicate names under the filesystem's applicable collation and namespace rules even if their original UTF-16 sequences differ. Handle exact-case-preserving names separately from comparison keys.
5. Reject or explicitly escape names that the target extraction platform cannot represent. Do not allow NUL, slash/path escape, or a Windows reserved basename to be silently mapped to the wrong file.

On a case-sensitive host, Windows Win32 naming restrictions (trailing spaces/periods; reserved device names; colon as stream separator; invalid control characters) differ from raw NTFS possibilities. For extraction, distinguish *raw on-disk name validity* from *destination-platform name validity*. Modern Windows also supports **case-sensitive directories**, including WSL-enabled use cases; raw NTFS images may contain distinct names with a common case-insensitive collation key when per-directory policy permits. Preserve such entries during reads, detect/record the directory's effective case-sensitivity semantics if available, and apply the conservative no-case-collisions rule only to fsimg's **default writer** profile, not to all valid read inputs ([Windows case-sensitivity guidance](https://learn.microsoft.com/en-us/windows/wsl/case-sensitivity)). For writing a broadly Windows-compatible image, adopt conservative Windows-compatible names by default; allow an opt-in forensic/raw namespace mode later.

### 6.6 Directory creation strategy

**P0:** generate only resident indexes. Order keys according to the selected `$UpCase` collation, create 8-byte-aligned entries, terminate the list, and fit the complete resident value within its MFT record. If a directory will not fit, fail before emitting any output or upgrade automatically to the independently tested P1 index-tree builder.

**P1:** size all key entries and partition them into fixed-size leaf index buffers. Generate parent separator entries and their child VCNs, propagating splits toward the root until it fits within a resident `$INDEX_ROOT`. Persist the child nodes as protected `INDX` records, with matching `$INDEX_ALLOCATION` runlist and `$BITMAP`. Recalculate fixups after every node's final contents are known. Every index key must be reachable exactly once (excluding intentional parent separators/duplicated keys according to verified B-tree semantics).

Keep directory index construction independent from the MFT attribute serializer. If a directory index spills from resident to nonresident, its MFT attributes, index-buffer allocation and volume bitmap all change; those changes belong in an iterative layout-planning phase, **not** in a streaming output write.

### 6.7 Hard links, parent references, and namespace enumeration

A hard link is a separate `$FILE_NAME` attribute on a **single MFT file** accompanied by an entry in the designated parent directory's `$I30`. The file's MFT header link count should agree with its actual supported hard links (not naively with the number of visible directory names if DOS aliases are present). NTFS cannot treat arbitrary directory hard links like POSIX file hard links. For extraction, deduplicate by stable MFT reference and create host hard links where supported. Detect cycles and prevent directory hard-link assumptions.

NTFS supports DOS 8.3 aliases. An implementation must preserve them as aliases in metadata when encountered, not extract them as additional independent file contents. P0 creation can omit generated 8.3 aliases if verified Windows and Linux readers accept that output.

References: [NTFS directory description](https://flatcap.github.io/linux-ntfs/ntfs/concepts/directory.html), [`$INDEX_ROOT`](https://flatcap.github.io/linux-ntfs/ntfs/attributes/index_root.html), [FreeBSD historical BSD-licensed NTFS source](https://cgit.freebsd.org/src/tree/sys/fs/ntfs), [NTFS-3G FAQ and filename notes](https://github.com/tuxera/ntfs-3g/wiki/NTFS-3G-FAQ).

---

## 7. Mandatory metadata files, special records, and layout requirements

### 7.1 Well-known initial MFT record numbers

The following names, numbers and roles are part of the NTFS v3.x interoperable format. They are *not* an instruction to clone binary metadata files from Windows or GPL-formatters.

| Record # | Canonical system object | Minimum reading/writing role |
| ---: | --- | --- |
| `0` | `$MFT` | Contains the complete MFT record stream; `$BITMAP` in this file marks allocated **MFT records** |
| `1` | `$MFTMirr` | File whose data stream mirrors the initial MFT records |
| `2` | `$LogFile` | Transaction log; actual replay is complex and excluded from fsimg P0 |
| `3` | `$Volume` | Holds `$VOLUME_NAME` and `$VOLUME_INFORMATION` |
| `4` | `$AttrDef` | Catalog of valid attribute kinds |
| `5` | `.` (volume root directory) | Root `$I30` index and associated metadata |
| `6` | `$Bitmap` | Data stream is a volume **cluster allocation bitmap** |
| `7` | `$Boot` | Maps NTFS boot-region sectors; boot sector also exists at fixed sector 0 and backup at volume end |
| `8` | `$BadClus` | Special file with bad-cluster bookkeeping, typically named stream `$Bad` |
| `9` | `$Secure` | Version 3.x shared security descriptor storage `$SDS`, indexes `$SII`, `$SDH` |
| `10` | `$UpCase` | 65,536-entry UTF-16 uppercase mapping used for filename collation |
| `11` | `$Extend` | Directory for optional system indexes/features (quotas, object IDs, reparse indexes, etc.) |
| `12..15` | reserved/system uses | Do not use for ordinary user content; formatting variants may have different conventions |
| `16+` | ordinary files and further metadata | Allocate through `$MFT:$BITMAP` and maintain references |

Do not assume metadata names are enough to identify a system object: the fixed MFT record number and attribute types/streams matter. Some later system files under `$Extend` can be allocated beyond the initial reserved range; a future NTFS reader must gracefully enumerate unknown ones.

### 7.2 `$MFT` and its own allocation bitmap

The `$MFT` unnamed `$DATA` stream is usually nonresident and must have enough storage for every allocated MFT record. Its `$BITMAP` is logically separate from `$Bitmap` record 6: a set bit means the corresponding MFT record number is allocated. The MFT may also have special metadata associated with its growth zone; for fsimg's offline writer, reserve contiguous MFT clusters and enough record slots for planned objects, all metadata files and possible attribute-extension records.

Bootstrap order for a reader:

1. Obtain initial `$MFT` LCN from the boot sector.
2. Read the first FILE record at that LCN and apply fixups.
3. Parse its unnamed `$DATA` runlist (and attribute list if needed).
4. Use that runlist for *all subsequent MFT record addresses*.
5. Validate record 0 identifies `$MFT`, `$Bitmap` within record 0 is usable, and the MFT data bounds agree with the image.

A creator must handle the self-reference without endless recursion: first allocate a location for the MFT; then synthesize the MFT record describing that location. The allocated-stream size may be larger than the number of in-use records. In bitmap creation, round and reserve MFT file space before generating its record.

### 7.3 `$MFTMirr`

The mirror's ordinary `$DATA` has a separate runlist, usually placed away from the MFT's primary extent. At minimum it contains the volume's expected bootstrap records (commonly the first four; some geometries result in larger coverage). Its contents must match corresponding protected MFT record **disk bytes** after the final USA values are inserted. Create the mirror last or synchronize it after any changes to records 0–3. Never regenerate the protected copies independently with different sequence tokens and then expect a byte-level mirror comparison to pass.

A reader may use the mirror for diagnosing damaged initial MFT records. By default fsimg should not silently substitute alternate metadata during routine extraction: report primary and mirror disagreement so the user knows that the image is suspect.

### 7.4 `$LogFile`: format-valid without implementing NTFS transactions

The log is **not** a random zero-filled filename placeholder. A valid NTFS log has restart areas and record-page organization, including signatures/protection and a state indicating whether replay is required. Its size and initialization requirements vary with Windows version and formatter choices. A newly generated offline volume has no mutations to replay, so fsimg should build a **clean, valid initial log**, never fabricate arbitrary committed journal entries.

A practical P0 writer should isolate a `logfile_init` module, implement a minimal initialization whose format is verified by multiple independent formatters and by Windows mount/`chkdsk`, and refuse unknown journal variants. No code or page images should be transplanted from non-BSD/proprietary sources. An apparently clean `$Volume` dirty bit is insufficient if the log replay state says otherwise.

For **reading**, report log/recovery state and refuse extraction requiring replay unless a documented best-effort snapshot mode is selected. fsimg is not a forensic journaling engine. Useful background: [NTFS `$LogFile` structure](https://flatcap.github.io/linux-ntfs/ntfs/files/logfile.html), [Microsoft transaction-logging overview](https://learn.microsoft.com/en-us/windows/win32/fileio/transactional-ntfs-portal).

#### 7.4.1 Log restart pages, record pages, and checks

The ordinary NTFS log file uses an LFS (Log File Service) container format. Common historical LFS 1.1 volumes begin with **two protected restart pages**, often 4 KiB each; the remaining log space holds protected log record pages and may include special tail copies. The stored major/minor format and page sizes govern interpretation. Do not assume all Windows generations use identical layout or initialization rules: recognize unsupported LFS versions and validate against actual Windows-format fixtures.

The following offset map is for the *historically documented LFS restart-page profile*; values are offsets from a page's own beginning. Its signature shares the USA-header layout with other protected NTFS records:

| Page-relative offset | Size | Field or validation question |
| --- | ---: | --- |
| `0x00` | 4 | Signature `RSTR` for restart page, or `RCRD` for log record page |
| `0x04`, `0x06` | 2 + 2 | USA location and count; protect/verify the complete page before interpreting its contents |
| `0x08` | 8 | `RSTR`: last checker LSN; `RCRD`: latest starting LSN or location represented by a tail copy |
| `0x10` | 4 | `RSTR`: system/restart page size; `RCRD`: page status flags |
| `0x14` | 4 (`RSTR`) / 2 (`RCRD`) | Restart page: log-record page size; record page: count of pages in transfer |
| `0x16` | 2 (`RCRD`) | Record page position within multi-page transfer |
| `0x18` | 2 | Restart page: relative restart-area offset; record page: first unused/current-record boundary |
| `0x1A`, `0x1C` | 2 + 2 (`RSTR`) | Minor and major LFS format versions |
| `0x20` | 8 (`RCRD`) | LSN of last record completed on the page |

For an `RSTR` page, the **restart area** (its offset supplied at restart-page `+0x18`) contains the following logical regions. Offsets in this table are relative to the *restart area*, not the page:

| Offset | Size | Meaning / parser use |
| --- | ---: | --- |
| `0x00` | 8 | Current LSN used in comparing candidate restart areas |
| `0x08..0x0D` | 6 | Count of client slots plus free-list and active-list heads (`0xFFFF` means no client) |
| `0x0E` | 2 | Restart flags; in this historical profile `0x0002` indicates clean dismount |
| `0x10` | 4 | Number of sequence bits in an LSN; required to interpret its address component |
| `0x14`, `0x16` | 2 + 2 | Restart area total length and offset of the client-record array |
| `0x18` | 8 | Declared length of the log file |
| `0x20` | 4 | Latest LSN's associated payload length |
| `0x24`, `0x26` | 2 + 2 | Log-record header size and start of log data inside a record page |
| `0x28` | 4 | Restart-area revision counter |

The client array conventionally identifies the NTFS log consumer; its entries refer to LSNs and contain an associated name. List heads must stay within the declared client count, and arrays, names, restart areas, page offsets and LSN-derived ranges must remain within the bounded log stream. Compare both restart pages only **after** validating their fixups and internal bounds. A clean-dismount bit is evidence of state, not a substitute for validating consistency with `$Volume` and the actual record pages.

In this historical profile, the per-record header within `RCRD` pages describes a current LSN (offset `0x00`, 8 bytes), previous-client and undo-chain LSNs (offsets `0x08` and `0x10`), client data length (`0x18`, 4 bytes), client identity (`0x1C..0x1F`), type (`0x20`), transaction identifier (`0x24`), and flags (`0x28`). The payload begins at `0x30`. Client **restart** and **redo/undo** records are different; record types alone do not establish that replay is complete. Full transaction replay requires checkpoint, open-attribute, dirty-page and active-transaction state and remains deliberately out of scope.

**Writer rule:** the above field map is for parsing and diagnostics, **not** a license to emit a simplistic all-zero or one-record journal. A valid clean initialization also depends on the chosen LFS version, matching redundant restart pages, sensible client metadata, protected-page trailers, log length and Windows' recovery expectations. Derive one explicit supported writer profile, document each initialized field, and pass Windows `chkdsk` before admitting it into P0. See the [public historical LFS field documentation](https://flatcap.github.io/linux-ntfs/ntfs/files/logfile.html) and the [libfsntfs format research](https://github.com/libyal/libfsntfs/tree/main/documentation) for comparative *format facts only*.

### 7.5 `$Bitmap`: authoritative volume-cluster allocation map

Every bit represents a volume LCN. Bit `i` is at byte `floor(i/8)`, bit `(i%8)`, least-significant bit first. A bit set to one means allocated/reserved. For a generated volume, it must cover **all** real allocations, including:

- MFT stream and MFT mirror;
- `$LogFile`, `$AttrDef`, `$UpCase`, `$Secure:$SDS`;
- system file streams (`$Boot`, `$BadClus`, metadata indexes and bitmaps);
- file data, directory `INDX` buffers, allocation metadata;
- any reserved or not-addressable tail clusters, depending on verified Windows formatter policy.

Never count clusters solely by traversing ordinary user-file `$DATA`: special streams also consume clusters. `$Bitmap` itself allocates clusters and thus must mark its *own* storage. That is a small dependency solved by allocating its clusters during the planning stage before serializing its bytes. Ensure that no allocated cluster appears under two ordinary independent streams, except for deliberately identical mirror/copied metadata which must be stored in **different** physical clusters.

### 7.6 `$Boot` and volume backup sector

The `$Boot` file describes boot-related data in an ordinary MFT record, but boot-sector recognition occurs at fixed volume offset 0. The backup boot sector resides at the final volume sector. For a data-only volume it is not necessary to deliver Microsoft boot code, but a syntactically recognizable NTFS boot sector and coherent backup are necessary. The reserved boot region must be accounted for in the volume cluster bitmap; don't let file allocations overwrite it.

**Open engineering question:** Windows-version-specific behavior for a nonbootable NTFS volume whose boot-code area is inert/zeroed. Validate with disposable Windows VHDX images, *not* on a boot disk. No proprietary boot image may be embedded to avoid this work.

### 7.7 `$BadClus`, `$AttrDef`, `$UpCase`, `$Extend`

- `$BadClus:$Bad` is special bookkeeping for unusable clusters and sparse/nonresident mapping semantics. A newly created virtual image with no known bad clusters should represent *none* as bad; use a formatter-compatible attribute setup rather than inventing “all blocks bad” mappings. In the documented NTFS 3.x profile, `$BadClus` has an empty **unnamed** `$DATA` stream and a named **`$Bad`** stream with a logical VCN range spanning the volume. That named stream is almost entirely *sparse*: non-hole runs identify unusable physical clusters; those clusters must also be reserved in volume `$Bitmap`. Therefore a very large logical `$Bad` length is normal even when it occupies essentially no clusters on disk. Confirm writer details against [the `$BadClus` on-disk description](https://flatcap.github.io/linux-ntfs/ntfs/files/badclus.html) and independent formatter fixtures.
- `$AttrDef` must contain usable, version-appropriate definitions for the attribute types actually emitted. A minimal invented list should not be assumed Windows-compatible until `chkdsk` verifies it.
- `$UpCase` must supply the exact collation mapping used for all serialized indexes. If the creator changes the uppercase table in a later release, that is an output-format compatibility change: indexes and collisions can change.
- `$Extend` is normally a system directory that contains optional NTFS services. Do not assume creating a bare empty record provides equivalent behavior to the Windows formatter: test creation of files after mount, object IDs, deletion and change notifications to establish whether omitted feature files are acceptable.

### 7.8 Recommended initial physical volume allocation

Physical placement is largely a writer policy, not a single standardized set of mandatory LCN offsets. The boot sector **dictates** where its MFT and MFT mirror are placed. NTFS commonly attempts to reserve space for MFT growth. A practical deterministic fsimg layout planner can allocate:

1. Boot area at LCN 0 with separate final-sector backup boot record; do not overlap the bitmap data extents.
2. Large, preferably contiguous MFT extent, sufficiently sized for complete known user tree and system records (plus headroom).
3. Mirror extent physically separated from first MFT extent.
4. Journal and metadata streams plus `$Secure` indexes and uppercase table.
5. User file data extents and large-directory `INDX` blocks.
6. Cluster bitmap last in *serialization order*, after all clusters are reserved (its physical location is allocated earlier).

Every physical address selected by the layout planner becomes a runlist entry in exactly the appropriate MFT attribute; a single layout ledger is the sole authority for cluster ownership. **A mountable NTFS image is not guaranteed by having recognizable `FILE` records; the entire metadata graph must be consistent.**

References: [NTFS special files](https://www.ntfs.com/ntfs-system-files.htm), [NTFS MFT](https://flatcap.github.io/linux-ntfs/ntfs/files/mft.html), [NTFS volume information](https://flatcap.github.io/linux-ntfs/ntfs/attributes/volume_information.html), [NTFS `$Secure`](https://flatcap.github.io/linux-ntfs/ntfs/files/secure.html).

---
## 8. Security descriptors, SIDs, ACLs, and security ID references

On NTFS 3.x, security is generally stored centrally. A file's extended `$STANDARD_INFORMATION` refers to a **numeric security ID**, whose corresponding self-relative Windows security descriptor is deduplicated in `$Secure:$SDS` and indexed through `$Secure:$SII` and `$Secure:$SDH`. A security ID is **not** an owner SID and is **not** a POSIX uid/gid. Treating the ID as an arbitrary dummy is one of the biggest risks in a new NTFS formatter.

### 8.1 Self-relative security descriptor (SD)

A serialized self-relative SD begins with a 20-byte fixed header; offsets are **relative to the beginning of the descriptor**:

| Offset | Width | Meaning |
| --- | ---: | --- |
| `0x00` | 1 | revision, normally 1 |
| `0x01` | 1 | reserved/resource-manager flags |
| `0x02` | 2 | control flags, with self-relative bit `0x8000` set |
| `0x04` | 4 | optional owner SID offset, or 0 |
| `0x08` | 4 | optional group SID offset, or 0 |
| `0x0C` | 4 | optional SACL offset, or 0 |
| `0x10` | 4 | optional DACL offset, or 0 |
| `0x14` onward | variable | owner, group, ACL objects, arranged at offsets specified in the header |

A security descriptor is neither an MFT attribute header nor a filename attribute. The self-relative bit is required for this serialized form. DACL-present and SACL-present flags control whether the corresponding ACL is meaningful. Crucially, a **null DACL** (unrestricted access semantics) is not the same as an **empty DACL** (deny access under ordinary Windows checks). Never invent ACL bytes or treat absent/empty as interchangeable.

### 8.2 SID and ACL structure checks

An SID encodes revision, subauthority count, a six-byte **big-endian authority**, and that many four-byte **little-endian subauthorities**. Its length follows from the count; check the length before examining any subauthority. An ACL header describes its revision, byte length and ACE count; ACEs are variable-length records, each with its own type, flags and length. Validate each ACE remains within the ACL and do not assume every ACE has the same optional layout. Unknown ACE types should be retained as opaque checked bytes or reported as unsupported, not silently truncated.

For extraction to Unix-like hosts, record Windows security descriptors/owner SID and ACLs in a portable metadata view. Lossless Windows ACL → POSIX mode or vice versa is impossible in general. An explicit policy can map an ordinary read-only flag to restrictive host write bits, but must not claim full ACL preservation. On re-creation, ask for a named security template/profile rather than guessing the Windows identity of host numeric UIDs.

### 8.3 `$Secure:$SDS` entry format

`$SDS` is a **named `$DATA` stream** on record 9. Each descriptor-bearing record uses a small fixed header before the variable self-relative SD:

| Offset | Width | Meaning |
| --- | ---: | --- |
| `0x00` | 4 | descriptor hash |
| `0x04` | 4 | security ID |
| `0x08` | 8 | byte offset of this entry in `$SDS` |
| `0x10` | 4 | total entry length including this 20-byte header |
| `0x14` | variable | self-relative security descriptor bytes |

Entries are normally aligned on 16-byte boundaries. The observed v3.x format stores redundant copies of descriptor-entry regions with a `0x40000` (256 KiB) separation and has 256-KiB boundary constraints. This duplicate-storage format must be planned and verified; do not simply put consecutive descriptors into a 4-KiB resident attribute and declare `$Secure` complete. The redundant blocks and indexes must agree with the actual byte offsets written.

### 8.4 `$SII` and `$SDH`

`$Secure` normally contains two named indexes:

- **`$SII`**: security-ID lookup. Index key is an integer security ID; value locates the corresponding SDS entry.
- **`$SDH`**: descriptor-hash lookup. Key combines hash and security ID; used to find and reuse equal descriptors.

Each index has an `$INDEX_ROOT`; when it grows, the corresponding `$INDEX_ALLOCATION` and `$BITMAP` are also present. Their key payloads and collation are **not** `$FILE_NAME` directory keys, so the directory-specific parser/serializer must be parameterized by key type rather than indiscriminately applying `$I30` logic to all indexes. `$SII` uses unsigned-integer collation; `$SDH` uses descriptor-hash collation. Collision handling requires *both* hash and content comparison; a hash collision does not mean descriptors are equal.

The descriptor hash algorithm and initial security IDs deserve a dedicated Windows-interop test. The hash is a derived index property, not a cryptographic integrity guarantee. Format interoperability should be established by round-tripping independently constructed descriptors and inspecting both indexes. Do not transplant either the hash implementation or complete `$Secure` blob from a non-BSD implementation into the project.

### 8.5 Security policy for a standalone creator

The minimal formatter should define an explicit `windows_security_profile` from first principles, e.g. an ordinary data-volume root with administrative/system access and inheritable ACEs sufficient for user file creation. Construct descriptors using documented SD/SID/ACL wire-format rules and verify their effective behavior on a **disposable Windows VM**. For non-Windows platforms, the writer can map a few internal permission classes to this profile; do not silently assert that POSIX ownership is represented.

**Gate to Windows-writable P0:** Windows must be able to mount, enumerate, read, **create and delete** a new file, and write a subdirectory without ACL failures; and `chkdsk` must report no index/SDS/SDH/SII repairs. For convenience, authors can use Windows APIs (e.g. SDDL conversion and `GetNamedSecurityInfo`) to create *test fixtures* or verify semantics; this does not mean embedding proprietary code in fsimg.

References: [Microsoft self-relative security descriptor](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-dtyp/7d4dac05-9cef-4563-a058-f108abecce1d), [Windows security descriptor string format](https://learn.microsoft.com/en-us/windows/win32/secauthz/security-descriptor-string-format), [`$Secure` file](https://flatcap.github.io/linux-ntfs/ntfs/files/secure.html), [security indexes explanation](https://www.ntfs.com/ntfs-permissions-file-structure.htm).

---

## 9. Sparse, compressed, encrypted, reparse and optional features

### 9.1 Sparse files

A file marked sparse can have virtual clusters with no physical allocations. Such a run appears in a mapping-pairs array with an LCN-delta field width of zero (§5). Reads return zero bytes for those regions without reading physical blocks. **Sparse physical coverage does not imply a sparse logical file** if an attribute's initialized-size boundary also applies. Maintain separate `allocated_bytes`, `logical_size`, `initialized_bytes`, `physical_used_clusters` and a `sparse` flag.

P1 read should accept arbitrary runs, including leading/middle/trailing holes. P1 writer should support explicit sparse ranges while keeping bitmap bits unset for holes. A raw image can be *sparse at the host filesystem level* even if its embedded NTFS contents are not sparse; those are completely different layers. Do not equate POSIX `SEEK_HOLE` on the image file with NTFS sparse extent metadata.

### 9.2 NTFS LZNT1 compression

Ordinary NTFS compression uses the **LZNT1** family. In the common profile, a compression unit spans `2^compression_unit_exponent` clusters, typically 16 clusters. With 4-KiB clusters this is 64 KiB of uncompressed logical data, normally broken into independently compressed **up to 4-KiB LZNT1 chunks**. Physical storage of a unit is either:

- uncompressed: full physical cluster run with ordinary content;
- compressed: initial physical clusters followed by sparse/virtual clusters representing the unit's saved space; or
- sparse: no physical clusters for a logically zero unit.

Thus a generic sparse-run decoder is insufficient for a compressed stream. Group the runlist into units, reconstruct stored compressed bytes, parse LZNT1 chunk headers, expand the compressed chunks and enforce the advertised logical output size. In LZNT1 each chunk has a 2-byte header with fields for stored length and a compression bit; token meaning changes with the current output position. The precise token-width and back-reference behavior is specified in Microsoft's openly published **MS-XCA** description. Use it as a behavior reference, not as a source of copied implementation code.

Validation must enforce output limits before allocating memory; reject impossible back-reference distances, truncated literals/tokens, invalid 4-KiB chunk size boundaries, compression units covering too many VCNs and decompression bombs. Track a strict unit output upper bound and timeout/work budget. Tests need zero-filled, incompressible, highly compressible, crossing-unit-boundary and fragmented/compressed files. A creator can omit compression entirely for P0/P1.

**Distinguish LZNT1 from WOF/dedup:** Windows Overlay Filter (WOF) uses reparse points and externally managed compressed backing (e.g. XPRESS or LZX), not simply a standard compressed `$DATA` runlist. Do not try to feed WOF bytes into an LZNT1 decoder.

References: [MS-XCA compression algorithm](https://learn.microsoft.com/openspecs/windows_protocols/ms-xca), [MS-XCA LZNT1 specification](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-xca/94164d22-2928-4417-876e-d193766c4db6), [Windows file compression information](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_compression_information).

### 9.3 Reparse points, junctions and symbolic links

A reparse point is a file or directory with `$REPARSE_POINT` data and corresponding file flags. The payload begins with a 32-bit **tag** and tag-specific length/data. Common Microsoft tags include symbolic links and mount points/junctions; other tags identify cloud placeholders, WOF, dedup, and proprietary filter behavior. The Win32 API supports a reparse data buffer with tag, data length and reserved field, then tag-specific UTF-16 path offsets and lengths for symbolic link/junction types. Third-party tags may require GUID-tagged buffers.

A safe extractor's default is to **record the reparse object without following it**. For supported symlink types, create a host symlink only when an explicit policy permits it, checking that the target does not escape the chosen extraction root. For unsupported tags, preserve raw tag + payload in the manifest as opaque metadata. Do not infer target file contents from a reparse-point backing file; reporting it as an ordinary complete file can lead to missing or incorrect data.

The volume-global `$Extend:$Reparse` index may be needed for complete write compatibility; treat its presence, tag-key collation, and consistency as part of any P2 writer acceptance. Read-only support can safely expose reparse attribute payloads without understanding every tag.

References: [Microsoft reparse points](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/reparse-points), [REPARSE_DATA_BUFFER](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_reparse_data_buffer), [Windows file streams](https://learn.microsoft.com/en-us/windows/win32/fileio/file-streams).

### 9.4 EFS and BitLocker

**Encrypting File System (EFS)** is a per-file feature represented by additional NTFS attributes/streams and encryption metadata, notably EFS-related `$LOGGED_UTILITY_STREAM`. Without authorized key material, fsimg must not promise plaintext extraction. A raw NTFS reader may be able to recover encrypted raw ranges while preserving them as ciphertext with a status marker. A Windows user with the right EFS key can provide decrypted files through Windows APIs; fsimg must not invent key management.

**BitLocker** wraps or transforms the underlying volume storage; encrypted BitLocker sectors are not ordinary readable NTFS sectors. Detection/decryption is a separate outer-layer concern. A BitLocker image should not be misdiagnosed as a malformed NTFS volume because the boot sector does not expose NTFS.

### 9.5 Object IDs, quotas, USN and transactional NTFS

Some volumes include objects such as `$Extend:$ObjId`, `$Extend:$Quota`, `$Extend:$Reparse`, and the change journal `$Extend:$UsnJrnl` with streams such as `$J` and `$Max`. They may be large/sparse and are not equivalent to ordinary user files. The structures include non-filename indexes with their own comparison rules. There may also be TxF artifacts and other service metadata. The reader should not silently exclude them from a *forensic/full metadata* inventory; ordinary file-tree extraction may hide them under an explicit policy.

Do **not** create a plausible-looking empty `$UsnJrnl` or `$Quota` file and assume Windows semantics; omit optional features when acceptable, and add each feature only after fixtures show which attributes/indexes are required. NTFS transactional logging and recovery are beyond the initial image-creation contract.

### 9.6 Extended attributes and attributes with unfamiliar types

`$EA` and `$EA_INFORMATION` represent NT-style extended attributes. Their format and size constraints differ from POSIX xattrs and may include required-for-understanding flags. A robust reader should preserve unknown attributes as `(type, name, resident? raw value or nonresident logical stream)` when safe. It should also tag content with *unsupported decoding* status rather than dropping it and claiming a lossless extraction. A creator can omit EAs until P2/P3 if its ordinary file modes do not need them.

### 9.7 Directory/file timestamps and flags

NTFS stores creation time, modification time, MFT metadata-change time, and last-access time; these are not identical to POSIX ctime/mtime/atime. Preserve all four in a manifest with full 100-nanosecond precision and original time-zone-independent UTC epoch interpretation. Directory index/name copies may contain stale caches. The writer should create consistent values across `$STANDARD_INFORMATION`, `$FILE_NAME`, and `$I30` entries; the reader should report discrepancies without overwriting one representation with the other. Windows may update access/change times and metadata after mounting, so test bytewise determinism before the Windows write pass, not after it.

---
## 10. Reader and extractor design

### 10.1 Functional units

An fsimg NTFS reader is best split into small testable layers instead of embedding everything in one “mount” routine:

| Unit | Input → output | Critical error checks |
| --- | --- | --- |
| `ntfs_geometry` | volume-offset bytes → sector/cluster/record geometry | overflow, impossible powers, offsets outside host image |
| `ntfs_fixup` | protected FILE/INDX bytes → verified logical copy | USA fully inside first fixup sector, mismatch, record size |
| `ntfs_mft_bootstrap` | boot metadata + initial record → MFT stream map | corrupt first FILE record, self-reference cycles |
| `ntfs_record` | record number → validated attributes | sequence, in-use flag, used-size and attr-chain boundaries |
| `ntfs_runlist` | mapping bytes → normalized physical/hole extents | delta sign, run overflow, VCN gaps |
| `ntfs_attr` | base ref, type, attribute name → logical stream | resident vs nonresident, attribute-list extents, duplicates |
| `ntfs_index` | index root + buffer stream → validated sorted entries | fixups, child pointers, cycles, bitmap disagreement |
| `ntfs_collate` | on-volume `$UpCase` + UTF-16 names → comparable keys | code-unit overflow, duplicate names, invalid suffixes |
| `ntfs_security` | security ID → SD bytes and portable view | `$SII`/`$SDH`/`$SDS` cross-references, ACL bounds |
| `ntfs_walk` | root file ref → graph of links/objects/streams | hard-link de-dup, invalid parent, malicious paths |
| `ntfs_extract` | graph + output sink → target files/manifest | traversal escape, ignored ADS, partial writes, symlink behavior |

None of these modules requires a writable filesystem image. Readers should preserve the original disk bytes for diagnostics and apply USA fixups to separate scratch buffers.

### 10.2 Open / probe / mount algorithm

1. Accept a **byte-oriented volume offset** and a bounded readable host image handle. If the caller provides a whole disk, use the existing partition-table layer to identify an NTFS partition; if no partition table, treat offset zero as a bare filesystem only under an explicit probe policy.
2. Parse the boot sector (§2). Compute every geometric product with checked integer operations. Reject out-of-volume extents and implausible sizes.
3. Read record 0 at boot `mft_lcn * cluster_bytes`, parse/fix up it, and establish `$MFT` stream mappings. Verify the mapping covers at least required first records.
4. Open records 1–11, validate identities and essential attributes. Use `MFTMirror` only as a cross-check / recoverable fallback in forensic mode.
5. Read `$Volume` version and dirty/recovery state; establish default extraction policy. A clean-looking dirty bit does not prove that journal replay isn't required.
6. Load `$UpCase` from record 10; check the full 128-KiB logical extent in the v3.x profile and validate sorted index order against it.
7. Load/validate `$Bitmap` lazily or eagerly depending on strict-validation setting. Check allocated run coverage during full-volume verification.
8. Return a read-only filesystem object. Do not touch the source during mounting.

The reader should support `--strict` (verify globally before extraction), `--normal` (verify encountered paths and records), and `--forensic` (best effort with explicit warnings). Never downgrade a data-integrity failure to success without recording it in the output status.

### 10.3 Attribute/stream resolution

To resolve `(base_file_ref, type, optional_attr_name)`, obtain the base record; gather its direct attributes; if an `$ATTRIBUTE_LIST` is present, parse it and fetch extension records with cycle guards; match instance IDs and names; stitch any continuation VCN ranges. Keep **identity** distinct from **location**: a single stream can have multiple MFT attribute records or runlist pieces, yet is conceptually one logical stream.

For a read spanning resident content, perform a bounded copy. For a nonresident stream, translate each requested byte range into VCN(s), locate corresponding physical or hole run(s), read only the necessary physical extents, and zero-fill holes/uninitialized tail. Do not expose bytes beyond initialized length, even when the corresponding allocated disk clusters contain data. On compressed content, pass the relevant compression unit through its own decompressor (§9.2) before slicing the requested logical bytes.

Important invariants:

- `stream logical read length <= logical_size - offset` with checked operations;
- every runlist part inside the **declared** enclosing attribute length;
- no two distinct logical extents claim the same VCN (unless a known alternative representation is explicitly supported);
- file reference sequence validated at every dereference;
- de-dup of extension records to prevent exponential recursion;
- capped memory and CPU for pathological `attribute-list → extension → attribute-list` cycles and index trees.

### 10.4 Directory walk and extract semantics

Enumerate root MFT record 5, traverse `$I30` using explicit depth/cycle bounds, and for each entry resolve its referenced base record. Do not synthesize `.` or `..` as real independent file copies. If a file has several names/parents, expose them as separate links to a single stable logical object; for extraction, optionally make host hard links. Preserve NTFS-internal timestamps and permissions in manifest metadata even when host attributes cannot express them.

Default traversal policy is **no following symlinks, reparse points or junctions**. Names must be checked against the extraction root as separate path components. A malicious NTFS name can include characters rejected by Windows APIs or special by the host; either encode losslessly to a collision-safe filename convention (with mapping manifest) or explicitly reject/exclude that entry. Prevent output `../` escapes, Unicode-normalization collisions on macOS, casefold collisions on case-insensitive hosts, Windows alternate-stream colon confusion, and duplicate file creation overwrites.

### 10.5 Lossless manifest representation (proposed)

A project-neutral extraction manifest should express at least:

```text
volume:
  ntfs_version: [3, 1]
  sector_bytes: 512
  cluster_bytes: 4096
  serial: <u64>
  dirty_or_replay_needed: <bool-or-unknown>
objects:
  - identity: {mft_record: <u64>, sequence: <u16>}
    kind: file | directory | reparse | unknown
    names: [{parent_identity: <ref>, utf16le: <bytes>, namespace: <n>}]
    timestamps_100ns: {created: <u64>, modified: <u64>, mft_changed: <u64>, accessed: <u64>}
    windows_attributes: <u32>
    security_id: <u32-or-null>
    streams: [{type: DATA, name_utf16le: <bytes-or-empty>, size: <u64>, sha256: <hex-or-null>}]
    reparse: {tag: <u32>, opaque_data: <bytes>} | null
    errors: [<structured-diagnostics>]
```

This is an **illustrative data schema**, not executable code and not mandated by existing fsimg APIs. In production, raw 16-bit name units should be available even if their Unicode text conversion is invalid; hashes should be computed from actual logical stream bytes, not raw compressed extents. For very large images, stream files and produce a manifest iteratively instead of retaining the entire graph in memory.

### 10.6 Read-only integrity inspection

A standalone `fsimg verify` command could compare:

- boot sector vs last-sector backup;
- MFT records 0..mirror coverage vs `$MFTMirr`;
- each occupied MFT bitmap bit vs `FILE` record in-use flag;
- recorded nonresident allocation vs `$Bitmap` bits;
- every directory entry's file ref/name/parent against its referenced record's `$FILE_NAME`;
- link counts against visible names, with DOS alias caveats;
- each `$SII` security ID vs an SDS entry, matching offset/size/hash;
- each `$SDH` hash/key vs SDS descriptor and `$SII`;
- index key sortedness under **that volume's** `$UpCase`;
- each `INDX` child against its index allocation bitmap;
- logical data extents vs initialized/logical size boundaries.

For a malformed external volume, some violations are warnings instead of fatal: file-name cached timestamps, intentionally orphaned/deleted records, journal-related transitional allocations, and historical metadata layouts may legitimately differ. Keep *strict generated-image invariants* separate from *best-effort forensic warnings*.

---

## 11. Offline NTFS image creator / formatter

### 11.1 Why not a stream-oriented builder

Unlike libarchive, NTFS needs random-access physical placements that are referenced by already serialized metadata. Moreover, the allocation bitmap must mark its own storage, MFT stream extents are self-referential, directory indexes refer to file references created later, and security IDs refer to a separately indexed descriptor store. Attempting to write an NTFS image while visiting source entries one by one is likely to create invalid cross references or require rewriting earlier sections.

fsimg can avoid this problem without implementing full mutable NTFS: use an **offline immutable volume builder** which accepts a manifest/content providers, constructs a graph, plans all allocations, then serializes the complete image.

### 11.2 Proposed state machine

| Phase | Input | Output | Assert before advancing |
| --- | --- | --- | --- |
| A. Ingest | source archive/tree/manifest | objects, names, streams, timestamps | no duplicate unsupported names, hierarchy acyclic |
| B. Normalize | object graph | NTFS-specific normalized graph | UTF-16 constraints, hardlink identity, size limits |
| C. Reserve IDs | normalized graph | fixed MFT record IDs and links | no user records in reserved slots; unique IDs |
| D. Size records | logical attributes | tentative FILE records, resident/nonresident decision | attribute offsets, USA space and record capacity |
| E. Layout | required stream lengths | cluster extent ledger / MFT and index capacities | no physical overlaps, enough space |
| F. Materialize metadata | IDs, layout, security profiles | record attributes, runlists, indexes, bitmaps | all references resolve; index nodes fit |
| G. Converge | updated record/stream sizes | stable layout | no new allocations; else return to D/E |
| H. Serialize | final graph/layout | raw sectors and protected record bytes | every physical segment written exactly where ledger requires |
| I. Audit | emitted image | clean image or reproducible failure | internal verification, optionally independent checkers |

Cycle D–G can be limited to a small number of iterations and should detect a nonconverging layout rather than producing a half-written image. In P0, contiguous allocation and preselected large-enough MFT space can eliminate most cycles. Prefer **fail-closed** on unsupported MFT-overflow scenarios rather than shipping corrupt output.

### 11.3 Object and stream allocation

Reserve fixed record IDs 0..15 first; assign ordinary objects monotonically in a stable traversal order. Preserve aliases/hard links by allocating **one MFT object per distinct file identity**, not one per pathname. Security IDs should be deduplicated by full serialized descriptor equality and assigned from an explicit policy. Decide resident vs nonresident for every stream based on the exact total attribute-list size and record slack after other required attributes, not a global magic threshold like “small means 700 bytes.”

For each nonresident stream:

1. Decide logical size, initialized size and physical allocation based on sparsity/compression profile.
2. Pick ordered extents from a volume-local allocator; prioritize a single contiguous run for P0.
3. Serialize mapping pairs from that *final* extent list, using minimal signed delta-width and tested sign extension.
4. If mapping-pairs bytes cause their MFT segment to overflow, replan extents and/or move to extension MFT records (P1), never truncate the list.
5. Mark physical LCNs in the central cluster ledger; reserve special streams and `$Bitmap`'s own clusters as regular owners with well-defined IDs.

For a host-sparse output raw image, physical zero-filled regions may be punched out **after** fsimg has emitted the logical byte view, but only when doing so leaves the same readable bytes; don't replace NTFS allocation semantics with image-file sparseness.

### 11.4 Serializing a FILE record

Create a cleared logical record of the chosen size; fill the header and first-attribute offset; reserve USA space; insert attributes in canonical order; serialize aligned resident values or nonresident headers/names/mapping pairs; insert attribute terminator; set used and capacity fields, in-use flags and link count; write record ID and correct sequence counter; apply USA transformation **last**. Use identical protected bytes for MFT mirror, not a separately regenerated copy with a new sequence. Record any per-record LSN choices and ensure they agree with the clean journal policy.

Writers should include unit tests that decode every emitted record and compare the parsed model to the input model *before* any external tools see it. A writer passing only its own reader is necessary but insufficient.

### 11.5 Directories and indexes

For every directory, determine children only after names/MFT IDs are final. Build `$I30` keys from the child `$FILE_NAME` attributes; sort by the chosen `$UpCase`; create a terminal index entry; form resident `$INDEX_ROOT` if it fits. For larger directories, allocate/protect additional `INDX` nodes, build `$INDEX_ALLOCATION` and its bitmap, and install correct child VCNs. Index allocation needs to occur before the final `FILE` record is protected. A directory's parent link and each child's parent reference must agree.

Because the root contains system metadata as well as user content, construct its visible entries deliberately. Verify whether special `$Extend` and other metadata names belong in the chosen root-index compatibility profile, and don't simply extract Windows' metadata tree and reuse it as a binary template.

### 11.6 Security and journal initialization

Create a small set of standard security descriptors from documented field formats, deduplicate to `$Secure:$SDS`, and construct `$SII/$SDH` lookups. Assign their IDs consistently to each extended `$STANDARD_INFORMATION`. Set appropriate inheritance and ownership per the declared Windows security profile. Independently validate that the Windows driver grants intended read/write rights in a VM. Construct a syntactically valid **clean** `$LogFile` per an independently verified formatter profile. Initialize NTFS version and volume flags consistently. A fake empty journal or fake security index is not a valid shortcut.

### 11.7 Generate bitmaps after all reservations

Once all MFT and physical placements are frozen:

- synthesize `$MFT:$BITMAP` for every occupied MFT record;
- synthesize `$Bitmap:$DATA` for every allocated physical LCN;
- synthesize per-index `$BITMAP` attributes for all allocated INDX slots;
- mark the actual extents of *each bitmap itself* and all other system streams;
- ensure extra bits beyond the addressable cluster/MFT record ranges follow a formatter-verified convention (generally marked unavailable to allocation).

Treat each bitmap as a distinct typed object. In particular, never reuse a cluster bitmap as a directory index bitmap. A straightforward linear scan should show zero cluster ownership conflicts and zero allocated extents whose bitmap bits are clear.

### 11.8 Output and finalization order

Write a temporary image and atomically promote it only after audit. Suggested logical ordering:

1. Decide full image length and optionally create host-sparse zero-initialized backing.
2. Write bulk user data and metadata stream payloads to their reserved physical extents.
3. Serialize FILE records and all INDX records after their content and USA tokens are final.
4. Mirror bootstrapping FILE records in `$MFTMirr`.
5. Write the final volume bitmap, MFT-record bitmap, and other special allocation bitmaps if not already written in step 2.
6. Write boot/backup sector fields containing all final geometry and MFT locations; reserve an inert/zero boot-code region for nonbootable data-profile images.
7. Reopen the emitted image **read-only** and run `fsimg verify --strict`, then third-party inspection. Only now mark the temporary result complete.

The physical order of writing is a convenience; the important invariant is that **metadata content is derived from the finalized graph**, not guessed during emission. Never modify the user's input disk image as the original creating operation.

### 11.9 Minimum success criteria

A P0 NTFS writer is accepted only after all of the following are true:

- fsimg reads back filenames, byte streams and all four timestamps from the emitted image;
- one or more independent Linux/macOS libraries/tools list and read the same files;
- Windows mounts the image read-only equivalent, can enumerate and read every file;
- Windows then mounts a **throwaway copy** read/write and can create, edit, rename, delete files and directories;
- subsequent Windows remount plus `chkdsk` reports no NTFS structural/security/index corrections;
- Linux ntfs3 and NTFS-3G can repeat independent read tests, and at least one can perform a simple write on a cloned image;
- no host/platform parser follows an out-of-range extent or permits output path escape when fed corrupt test images.

The image need not be byte-identical to one produced by Windows or `mkntfs`; many physical layouts are valid. A clean independent checker outcome and a correct semantic tree are the meaningful compatibility tests.

---

## 12. fsimg API and user-facing semantics

The exact CLI and C API names below are **proposals**; they do not claim to exist in the current fsimg repository.

### 12.1 Uniform filesystem plugin shape

Core fsimg needs seekable `read_at`, optional bounded `write_at`, size, allocator and temporary-output callbacks. NTFS-specific plugins should be adapters over a common image/entry/stream model. The NTFS writer must not require a special temp-file copy for each source member; it can accept length-known or spooled streams from a generic content-provider interface and write them straight into planned extents.

Proposed conceptual API operations:

- `probe(image, offset)` → confidence, geometry and NTFS version;
- `open_read(image, offset, policy)` → readonly NTFS view;
- `enumerate(view, directory)` → entries with stable identity;
- `stream_open(view, file, type, name)` → seekable content view;
- `extract(view, target, policy)` → file tree and optional metadata manifest;
- `plan_create(objects, volume_profile)` → immutable layout plan with error list;
- `create_image(plan, output)` → complete volume bytes;
- `verify_image(image, strictness)` → diagnostics and structural consistency audit.

For filesystem-level creation, the full input tree is needed (even when its contents are streamed later), because exact NTFS metadata sizes depend on names, tree structure, file sizes and optional streams. This is **planning**, not a requirement to buffer every file's contents in RAM.

### 12.2 Candidate CLI syntax

```sh
# Names/flags are proposed, not necessarily implemented yet.
fsimg info --type ntfs --offset 0 volume.ntfs
fsimg list --type ntfs volume.ntfs
fsimg extract --type ntfs --manifest ntfs-meta.json volume.ntfs ./out
fsimg create --type ntfs --size 128M --cluster-size 4096 \
  --label FSIMGTEST --security-profile windows-data ./src ./test.ntfs
fsimg verify --type ntfs --strict ./test.ntfs
```

The API should support bare NTFS volume images independently from the whole-disk/partition wrapper. In partitioned mode, fsimg should either emit a GPT/MBR partition outside the filesystem formatter or accept a caller-provided volume offset. Never make the NTFS formatter invent an MBR/GPT layout implicitly.

### 12.3 Representation and portability policies

- **Names:** preserve UTF-16LE code units, namespace, original displayed form and case-insensitive comparison key. Do not normalize without recording changes.
- **Links:** hard links refer to a common logical object; DOS 8.3 aliases are metadata; reparse points are inert by default.
- **Streams:** ADS are first-class streams attached to a file, not additional top-level files.
- **Permissions:** explicit Windows-security vs POSIX-best-effort policy; report dropped ACL semantics.
- **Times:** four NTFS timestamps, integer 100-ns tick representation; don't overwrite MFT-change time with POSIX ctime.
- **Sparse/compressed:** report content transformation status, physical storage separately, and fail on unsupported decoding rather than returning compressed bytes as file contents.
- **Dirty images:** refuse when safety policy demands clean snapshot; offer forensic inspection with warnings.
- **Malformed metadata:** every failure includes location (`volume_offset`, `mft_record`, attribute, VCN/LCN when known), kind, and severity.
- **Output determinism:** fixed traversal ordering, stable IDs, fixed security profile, known Unicode collation version and optional random/seeded serial; no hidden host locale/time dependencies.

---
## 13. Testing tools, reference writers/readers and reproducible recipes

**Safety:** all formatting, mounting, fixing and copying commands in this section are intended for newly created, disposable test image files only. Never direct `mkntfs`, Windows Format, `chkdsk /f`, `ntfsfix`, partition tools or writable loop mounts at an actual disk holding valuable data. Give each oracle **its own copy** of the image so one filesystem driver cannot affect another driver's results.

### 13.1 Tool capability table

| Tool / platform | Read/list/extract | Create/format | Modify contents | Verify/repair | Notes |
| --- | --- | --- | --- | --- | --- |
| **Windows `ntfs.sys`** via mounted VHDX | Yes | `format /FS:NTFS`, `Format-Volume` | Full OS file APIs | `chkdsk /scan`, `chkdsk /f`, `fsutil fsinfo` | Essential real-world acceptance oracle; Windows modifies metadata on writable mounts |
| **`ntfs-3g` (FUSE)** | Yes | — | Yes | `ntfsfix` limited repair | Independent userspace implementation; GPL, **tool only**, no code reuse |
| **`mkntfs` / `mkfs.ntfs` (ntfsprogs)** | N/A | Yes | — | — | Excellent golden image producer; format to regular files; GPL |
| **`ntfsls`, `ntfscat`, `ntfsinfo`, `ntfscp`** | Yes | — | `ntfscp` to offline NTFS | structural diagnostics | CLI targets for regression comparison; GPL |
| **Linux kernel `ntfs3`** | Yes | — | Yes via mount | kernel metadata checks; external utilities for repair | Independent kernel read/write oracle; GPL kernel |
| **FreeBSD historical `sys/fs/ntfs`** | Legacy read support | No | Not suitable | — | BSD-family licensed source reference; may not build on current FreeBSD |
| **libyal `libfsntfs` / `fsntfsinfo`** | Yes | No | No | inspection, not repair | LGPLv3+, strong independent metadata/compression oracle; has WOF read support |
| **libyal `fsntfsmount`** | Read-only FUSE-style view | No | No | — | Independent extraction oracle; can open offset inside raw disk |
| **ColinFinck `ntfs` Rust** | Yes, incl. ADS | No | No | parser errors | MIT/Apache-2.0; `ntfs-shell` browsing tool |
| **`rust-fs-ntfs` (2026)** | Yes | `mkfs.ntfs` CLI | File/dir/stream writes, selected features | `fsck.ntfs` targeted consistency tool | MIT/Apache-2.0, rapidly evolving; check its capability matrix and implementation limits |
| **Sleuth Kit (`fsstat`,`fls`,`istat`,`icat`)** | Yes; forensic views | No | No | inspect allocation and deleted entries | Mixed licenses, including IBM Public License; independent forensic perspective |
| **`qemu-img`** | Image conversion | VHDX container only | Converts/writes container | `qemu-img info` | Does **not** create NTFS; conversion is for Windows testing |
| **Windows Disk Management / DiskPart** | Yes via mounted volumes | GPT/MBR partition and NTFS format | Yes | status | Be extra careful to select only the attached disposable VHDX |

No one tool validates all facets. For example, `ntfsfix` **is not** a Linux replacement for `chkdsk`, and an NTFS-aware extractor can succeed while Windows still rejects the volume's security descriptor or transaction log. Use independent read-only parsers first, then a Windows `chkdsk`/writable-mount test on a copy.

### 13.2 Produce an independent NTFS golden image with NTFS-3G tools (Linux)

```sh
# In an EMPTY temporary directory, for a throwaway golden fixture.
truncate -s 128M reference.ntfs
mkntfs -F -Q -L FSIMGREF -s 512 -c 4096 reference.ntfs

# Populate an ordinary file and a named data stream while still unmounted.
printf 'hello from an independent NTFS writer\n' > host-hello.txt
printf 'independent ADS bytes\n' > host-meta.txt
ntfscp reference.ntfs host-hello.txt /hello.txt
ntfscp -N meta reference.ntfs host-meta.txt /hello.txt

# Inspect the independent formatter's output.
ntfsinfo -m reference.ntfs
ntfsls -a -p / reference.ntfs
fsntfsinfo -H reference.ntfs          # if libfsntfs tools are installed
fsstat -f ntfs reference.ntfs          # if Sleuth Kit is installed
fls -f ntfs reference.ntfs
```

`mkntfs -F` explicitly permits the regular-file target for this golden-fixture scenario, and `-Q` requests quick formatting. Verify the installed man page for version-specific behavior. Do **not** bundle bytes from the golden image as embedded fsimg metadata templates: inspect only, use independent validation, and generate your own structures.

A golden corpus should contain the same logical files from **multiple** independent creators: Windows Format, `mkntfs`, and the actively maintained Rust formatter when it passes tests. Do not treat agreement with a single writer as a spec.

### 13.3 Read data and ADS with offline tools

```sh
ntfsls -a -p / reference.ntfs
ntfsinfo -m reference.ntfs
ntfsinfo -i 5 reference.ntfs                 # root record: useful structural view
ntfscat reference.ntfs /hello.txt > hello-from-ntfs.bin
ntfscat -n meta reference.ntfs /hello.txt > meta-from-ntfs.bin

fsntfsinfo -E 5 reference.ntfs             # inspect selected MFT record
fsntfsinfo -H reference.ntfs             # hierarchy
fsstat -f ntfs reference.ntfs
fls -r -f ntfs reference.ntfs
istat -f ntfs reference.ntfs 5
# With real file ref/attribute numbers obtained from istat:
# icat -f ntfs reference.ntfs '42-128-1' > recovered-stream.bin
```

Not every NTFS CLI interprets path and ADS syntax identically. Use `--help` and metadata-addressing documentation rather than assuming colons are handled uniformly. In Sleuth Kit an MFT stream can be selected by `MFT-record-attribute-type-attribute-id`, for example `42-128-1` for the unnamed data stream if that's the actual instance on the fixture.

`ntfscp` can write into a **separate, unmounted scratch image**:

```sh
cp reference.ntfs writer-ntfscp.ntfs
printf 'NTFS tool write test\n' > local.txt
ntfscp writer-ntfscp.ntfs local.txt /hello.txt
ntfscat writer-ntfscp.ntfs /hello.txt
```

For directory creation or named stream creation that `ntfscp` cannot perform alone, use an `ntfs-3g` FUSE mount or Windows. Don't let two drivers mount/write the same image concurrently.

### 13.4 FUSE userspace read/write tests (Linux)

```sh
# Copy a valid fixture first; requires ntfs-3g / fuse setup and suitable perms.
cp reference.ntfs fuse-test.ntfs
mkdir -p ./mnt-ntfs
ntfs-3g -o loop,rw ./fuse-test.ntfs ./mnt-ntfs

mkdir ./mnt-ntfs/nested
printf 'contents\n' > ./mnt-ntfs/nested/example.txt
mv ./mnt-ntfs/nested/example.txt ./mnt-ntfs/nested/renamed.txt
sync
fusermount3 -u ./mnt-ntfs 2>/dev/null || fusermount -u ./mnt-ntfs

ntfsls -a -p /nested fuse-test.ntfs
ntfscat fuse-test.ntfs /nested/renamed.txt
```

Some platforms mount image files directly without the `loop` option and some mount commands need `sudo`; adapt locally. **Mount only disposable files, not active Windows system volumes.** Tests should include clean unmounts and subsequent independent reads. Unclean shutdown/hibernation/fast-startup states are intentionally separate from the clean creator profile.

### 13.5 Linux kernel `ntfs3` independent read/write

```sh
cp reference.ntfs kernel-ntfs3.ntfs
LOOP=$(sudo losetup --find --show kernel-ntfs3.ntfs)
mkdir -p ./mnt-ntfs3
sudo mount -t ntfs3 -o rw "$LOOP" ./mnt-ntfs3
# Perform an isolated write test, then flush/unmount:
printf 'from kernel ntfs3\n' | sudo tee ./mnt-ntfs3/kernel.txt >/dev/null
sync
sudo umount ./mnt-ntfs3
sudo losetup -d "$LOOP"

ntfscat kernel-ntfs3.ntfs /kernel.txt
```

If the kernel lacks `ntfs3`, skip this oracle rather than falling back silently to `ntfs-3g` while reporting kernel coverage. On systems where `losetup` interprets relative paths differently, use an absolute image path. If test media is mounted by any driver, avoid raw writes to the underlying file.

### 13.6 Actively maintained MIT/Apache-licensed Rust formatter/driver

The repository documents one CLI binary with several installed command names. Confirm its README/install helper for the checked-out revision:

```sh
# In the cloned rust-fs-ntfs repo; tests/oracles, not source imports into fsimg.
cargo build --release --features cli --bin rust-fs-ntfs
# The project documents an installer for convenience command aliases:
chore cli:install
export PATH="$PWD/tmp/cli/bin:$PATH"
mkfs.ntfs --size 64M --label BACKUP rust-reference.ntfs
fs.ntfs rust-reference.ntfs ls /
fs.ntfs rust-reference.ntfs stat /
fsck.ntfs rust-reference.ntfs

# Once a test file exists:
# fs.ntfs rust-reference.ntfs read /notes.txt > notes.txt
# fs.ntfs rust-reference.ntfs write /notes.txt < notes.txt
# fs.ntfs rust-reference.ntfs mkdir /backup
```

`rust-fs-ntfs` evolves quickly. Its October 2026 documentation still identifies limitations on attribute lists, fully general index-block insertion, sparse/compressed streams and MFT self-growth. Use its formatter and test matrix as a **second oracle**, but do not claim it is a feature-complete substitute for Windows or `ntfs-3g`.

Other permissively licensed parser CLIs: [ColinFinck ntfs-shell instructions](https://github.com/ColinFinck/ntfs#ntfs-shell). The `ntfs-shell` example is read-only and is useful for drilling into records, indexes and named streams without writing the source image.

### 13.7 Test fsimg's NTFS volume under actual Windows using VHDX

Windows disk-image mounting ordinarily expects a virtual **whole disk** with a partition table, while fsimg's formatter may emit a **bare NTFS volume**. These are distinct files. First, put the tested 128-MiB NTFS volume at a 1-MiB offset in a larger disposable GPT raw disk, convert the **whole disk** to VHDX, then mount that VHDX in a Windows VM. The example below assumes `fsimg-output.ntfs` is **exactly 128 MiB and has 512-byte logical sectors**:

```sh
# Linux-side fixture preparation; use disposable files, never real devices.
test "$(stat -c%s fsimg-output.ntfs)" -eq 134217728 || exit 1
truncate -s 256M ntfs-disk.raw
sgdisk --zap-all ntfs-disk.raw
# 262,144-sector NTFS partition: LBA 2048..264191 inclusive.
sgdisk -n 1:2048:264191 -t 1:0700 -c 1:FSIMGTEST ntfs-disk.raw
dd if=fsimg-output.ntfs of=ntfs-disk.raw bs=1M seek=1 conv=notrunc status=progress
qemu-img convert -f raw -O vhdx ntfs-disk.raw ntfs-disk.vhdx
qemu-img info ntfs-disk.vhdx
```

The embedded NTFS boot sector's reported total sectors must not exceed the GPT partition's size. For other sizes, compute the end LBA rather than using this exact fixed example. The GPT `Microsoft basic data` partition type is `0700` in `sgdisk`. An NTFS volume with zeroed boot *code* can still be a data volume, but Windows interoperability must be tested empirically. If Windows requests formatting the mounted partition, **cancel**: that is a test failure, not an instruction to repair the source.

On **Windows**, open an elevated PowerShell window with a throwaway `C:\temp\ntfs-disk.vhdx` that has been copied from the above file:

```powershell
$Image = 'C:\temp\ntfs-disk.vhdx'
Mount-DiskImage -ImagePath $Image
Get-DiskImage -ImagePath $Image | Get-Disk | Get-Partition | Get-Volume

# Identify the drive letter allocated to *the VHDX*, not your system disk.
# Suppose that volume appears as E:. Do not blindly use E: on your machine.
fsutil fsinfo ntfsinfo E:
fsutil fsinfo volumeinfo E:
chkdsk E: /scan

# On this scratch volume only, check read/write interoperability:
New-Item -ItemType Directory -Path 'E:\fsimg-test' | Out-Null
Set-Content -LiteralPath 'E:\fsimg-test\hello.txt' -Value 'hello from Windows'
Set-Content -LiteralPath 'E:\fsimg-test\hello.txt' -Stream 'details' -Value 'named stream'
Get-Item -LiteralPath 'E:\fsimg-test\hello.txt' -Stream *
Get-Content -LiteralPath 'E:\fsimg-test\hello.txt' -Stream 'details'
Rename-Item -LiteralPath 'E:\fsimg-test\hello.txt' -NewName 'renamed.txt'
fsutil hardlink create E:\fsimg-test\hardlink.txt E:\fsimg-test\renamed.txt
chkdsk E: /scan

Dismount-DiskImage -ImagePath $Image
```

For additional clean/repair checks, always start with a **copy of the VHDX**. Running `chkdsk /f` changes the image and may hide bugs by fixing your formatter's mistakes. Capture its output and any structural corrections as test failures on an unmodified fsimg output, then analyze the repaired copy to understand what was missing. Run readback from `ntfs-3g` and fsimg after Windows changes the volume: Windows may update indexes, journal state, short names and timestamps.

Windows can create its own independent format oracle with Disk Management, a disposable VHDX, and `format /FS:NTFS /Q` against **the confirmed VHDX partition drive letter**. Never provide a prefilled hard-coded drive-letter `format` command in CI. Compare semantic structure, not literal bytes, since layout, log sizes, security IDs and timestamps vary.

### 13.8 Cross-check `ntfsfix` and full volume repair cautiously

`ntfsfix` may correct certain basic issues and schedule Windows checking, but it does **not** perform the full consistency checks or reconstruction of Windows `chkdsk`. It must never be used as the only validator for writer acceptance. Include its result as optional *additional* diagnostic evidence on a throwaway clone. Windows checks should record output of **both** `chkdsk /scan` and a separate isolated `chkdsk /f` run where feasible, checking explicitly whether repairs were performed.

### 13.9 macOS / FreeBSD hosts

- On macOS, use compiled command-line userspace tools (`fsntfsinfo`, `ntfs-3g` where maintained/available, Rust readers, Sleuth Kit) to inspect raw NTFS volumes. Native macOS mounting typically provides at most a limited read-only validation surface; don't depend on undocumented third-party write flags.
- On FreeBSD, the historical NTFS code is a valuable **BSD-license code reference**, not a promise of a working modern kernel NTFS write driver. Prefer a Linux VM, Windows VM, or cross-platform Rust/libfsntfs tools as the active oracles.
- On all hosts, make sure tools have the necessary image offset. For example `fsntfsinfo -o` takes a **byte offset**, whereas Sleuth Kit's `-o` expects a **sector offset** (see its tool-specific help). Unit confusion is a common source of false corruption reports.

References: [ntfs-3g manual](https://github.com/tuxera/ntfs-3g/wiki/Manual), [`mkntfs` man page](https://manpages.debian.org/trixie/ntfs-3g/mkntfs.8.en.html), [`ntfscp` man page](https://manpages.debian.org/unstable/ntfs-3g/ntfscp.8.en.html), [`ntfscat` man page](https://manpages.debian.org/testing/ntfs-3g/ntfscat.8.en.html), [Sleuth Kit tool overview](https://wiki.sleuthkit.org/TSK-Tool-Overview/), [Windows `Mount-DiskImage`](https://learn.microsoft.com/powershell/module/storage/mount-diskimage), [`Get-DiskImage`](https://learn.microsoft.com/en-us/powershell/module/storage/get-diskimage), [`chkdsk`](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk), [`fsutil fsinfo`](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-fsinfo), [Rust CLI documentation](https://docs.rs/crate/rust-fs-ntfs/latest).

---
## 14. Test matrix, fault injection and specification-guided fuzzing

The first objective is proving **byte-safe parsing** of hostile disk images; the second is **semantic compatibility** of image creation. These are different test suites. A formatter output accepted by its own reader proves only self-consistency, not Windows compatibility.

### 14.1 Minimum test corpus by producer

Maintain separately versioned, SHA-256-identified fixture sets:

- **W:** disposable images formatted and populated by supported Windows releases, including an NTFS 3.1 volume with files created through Win32/PowerShell APIs and a **4Kn** variant if supported by the test VM stack;
- **G:** `mkntfs` NTFS-3G images with varied cluster and volume sizes and populated through `ntfs-3g`;
- **R:** `rust-fs-ntfs` formatter images, where the tagged CLI version and compatibility matrix are captured;
- **F:** deterministic fsimg-generated output, both raw filesystem and GPT/VHDX-wrapped whole-disk variants;
- **M:** **mutated** small fixtures containing precisely one violation per test, plus a few compound-corruption examples;
- **H:** forensic/historical images of earlier NTFS 3.0 and 1.x variants with known feature provenance (read-only expectations only).

Never check in proprietary boot code or private files into source control. Store test generators/fixture recipes and independently synthesized payloads where possible. If a proprietary Windows-generated test image must be retained in CI infrastructure, keep it outside the source specification and review its redistribution, privacy and licensing separately. Golden images used as input to tests are not an authorization to copy their data structures as writer templates.

### 14.2 Concrete feature matrix

| ID | Tier | Input condition | Required read behavior | Required create/interop check |
| --- | --- | --- | --- | --- |
| G01 | P0 | 512-B logical sectors, 4-KiB clusters | correct volume recognition | `fsutil ntfsinfo` matches geometry |
| G02 | P1 | 4,096-B logical sectors (4Kn) | correct geometry/fixup handling | Windows 4Kn VHD/device profile if feasible |
| G03 | P1 | 512-B clusters | valid record and file reads | external formatter and independent inspection |
| G04 | P1 | 64-KiB clusters | correct arithmetic/stream mapping | selected Windows-supported size; compression policy |
| G05 | P0 | 1,024-B FILE records | parsed attributes/fixups | default fsimg output works |
| G06 | P1 | 4,096-B FILE records | parsed record length / USA count | non-default format profile if selected |
| G07 | P1 | `INDX` size different from cluster size | child address correctly decoded | external index tests |
| G08 | P0 | primary and backup NTFS sectors | compare/diagnose | backup matches emitted boot |
| G09 | P1 | damaged primary sector but good backup | diagnostics/forensic recovery | no automatic alteration |
| R01 | P0 | small resident file | exact bytes, length and hash | Windows reads same bytes |
| R02 | P0 | empty file / empty resident stream | exists, size 0 | Windows creates/removes zero-byte file |
| R03 | P0 | file body just exceeds resident threshold | nonresident exact logical bytes | record still fits, Windows reads |
| R04 | P0 | byte lengths near cluster boundaries | correct initialized tail | Windows hash matches input |
| R05 | P1 | fragmented `$DATA` across many extents | exact range reads and hash | optional fragmented writer profile |
| R06 | P1 | runlist with signed negative LCN delta | correctly signed physical mapping | metadata independent parser acceptance |
| R07 | P1 | one or more sparse runs | zero-filled holes, correct size | Windows `FSCTL_SET_SPARSE` oracle |
| R08 | P1 | initialized length less than logical size | zero-filled uninitialized bytes | no stale physical data leaks |
| R09 | P2 | compressed LZNT1 file | correct logical bytes and hash | writer optional; Windows compression oracle |
| R10 | P3 | WOF/LZX or XPRESS backed file | explicit supported/unsupported result | no ordinary-data masquerade |
| M01 | P0 | record 0 bootstrap and MFT runlist | resolve first and later records | all system MFT record identities valid |
| M02 | P0 | MFT mirror populated | detect mismatch | mirror agrees byte-for-byte as profile requires |
| M03 | P0 | MFT bitmap with many in-use records | full record enumeration | bitmap and in-use flags consistent |
| M04 | P0 | volume `$Bitmap` allocation | no leaked/overlap extents | allocation totals agree with checker |
| M05 | P0 | valid initial transaction log | clean-volume diagnostic | no Windows log replay errors |
| M06 | P0 | `$UpCase` 128 KiB | correct collation | Windows accept names and directory indexes |
| M07 | P0 | `$AttrDef` generated from spec | recognized metadata types | `chkdsk` no attribute-definition repairs |
| M08 | P0 | `$Secure` descriptors and indexes | security ID resolves correctly | Windows create/delete checks ACLs |
| I01 | P0 | root small `$INDEX_ROOT` directory | all names once, sorted correctly | Windows enumeration and creation |
| I02 | P1 | large `$INDEX_ALLOCATION` index | complete recursive walk | Windows large-directory operations |
| I03 | P1 | multi-level directory tree | child VCN traversal, no cycles | 10k+ entries, check sortedness |
| I04 | P1 | index bitmap sparse/free slots | skip free, diagnose bad pointers | rescan after deletion/reinsertion |
| I05 | P1 | many small/variable UTF-16 name keys | name boundary checks | no invalid key sorting |
| N01 | P0 | ASCII/Unicode mixed case | preserve original, case-insensitive find | case-collision behavior against `$UpCase` |
| N02 | P1 | BMP Unicode and surrogate pairs | preserve raw UTF-16; no truncation | Windows path round-trip where representable |
| N03 | P1 | DOS 8.3 aliases | no duplicate extracted file | reader retains alias relation |
| N04 | P1 | same file hard-linked in two directories | identity shared, link preserved | Windows link count matches |
| N05 | P1 | two names colliding after host normalization | collision-safe extraction | creator rejects unsupported collisions |
| A01 | P1 | one named ADS | read unnamed + named separately | Windows `Get-Item -Stream *` works |
| A02 | P1 | empty ADS and multiple ADS | distinguish empty from absent | Windows re-read all streams |
| A03 | P1 | `$ATTRIBUTE_LIST` extension record | resolve split attributes | writer can fail safely if not implemented |
| A04 | P1 | fragmented MFT and attribute extensions | record lookup across extents | corpus read from Windows-created image |
| A05 | P2 | `$REPARSE_POINT` symlink or junction | expose inert tag/payload | optional creation; no unsafe traversal |
| A06 | P2 | extended attributes | preserve declared value/opaque status | optional Windows/WSL EA oracle |
| A07 | P2 | security descriptor inheritance | parse/check SID/ACE bounds | Windows ACL and deny/allow behavior |
| A08 | P2 | valid alternate index types (`$SII`, `$SDH`) | distinct key comparisons | security index no chkdsk repairs |
| A09 | P3 | EFS-encrypted file | explicit encrypted/unsupported status | no plaintext promise |
| A10 | P3 | older NTFS version | error or supported behavior by fixture | no writer guarantee |
| X01 | P0 | fsimg NTFS raw image | enumerate/extract | Linux ntfs-3g + libfsntfs agree |
| X02 | P0 | fsimg NTFS in GPT/VHDX | volume offset correct | Windows mount + read + write on cloned image |
| X03 | P0 | internal writer-readback hash | all file bytes match | independent reader hashes also match |
| X04 | P0 | Windows clean `chkdsk /scan` | structural success | no repairs prompted on clean output |
| X05 | P0 | Windows file create/rename/delete | read after Windows edits | second independent driver reads results |
| X06 | P1 | repeated mount cycles and unmounts | valid new metadata | no accumulation of `chkdsk` corrections |
| X07 | P1 | same logical manifest, different allocation plan | identical semantic reads | no physical-layout dependency |
| X08 | P1 | same manifest and seed twice | stable deterministic image bytes | no hidden clocks/locale/randomness |
| X09 | P1 | nearly full volume / insufficient space | clean fail with ENOSPC | no partial misleading “valid” result |
| X10 | P1 | MFT record capacity exhaustion | controlled fail/EFBIG | no invalid attribute truncation |

Set explicit quantitative ceilings for parser runtime and memory (e.g. a per-fuzz-case 100-ms time budget and ~256-MiB process limit on small fuzz volumes). Keep full Windows integration checks separate from millisecond-scale parser fuzzing: running `chkdsk` for each random mutation is neither practical nor safe.

### 14.3 Specification-derived negative mutation matrix

Generate one mutation from each constraint class below, and assert that fsimg returns a **typed error or forensic warning without memory-safety violations**:

| Class | Target fields / boundary | Expected invariant |
| --- | --- | --- |
| B00 | sector size zero, nonpower of two, absurd value | reject before multiplying |
| B01 | sector count greater than image, zero clusters, impossible MFT LCN | reject before seek/read |
| B02 | signed size code zero/extreme exponent | reject overflow / invalid size |
| B03 | boot MFT LCN pointed into backup sector/out of range | fail bootstrap safely |
| F00 | `FILE`/`INDX` signature changed | invalid protected-record type |
| F01 | USA offset/count underflows/overflows first 512-byte sector | reject without out-of-bounds memory |
| F02 | one sector trailer token differs | detect torn-write corruption |
| F03 | first attribute offset before header / after used bytes | reject malformed attribute chain |
| F04 | record used size > allocated size, or no end marker | reject record |
| F05 | sequence counter stale in index/parent ref | report stale reference |
| T00 | attribute length zero, 7-byte/negative-style, > record | reject non-progress/corruption |
| T01 | name offset/name length outside attribute | bounds-check before UTF-16 decode |
| T02 | resident content offset/length too large | reject before copy |
| T03 | nonresident highest VCN < lowest VCN | reject inconsistent interval |
| T04 | stream initialized_bytes > logical_bytes | reject invalid stream |
| T05 | unknown attribute type | skip safely by validated length |
| V00 | run count zero or run header nibble width > 8 | reject mapping pair |
| V01 | truncated run count/delta bytes | reject before reading |
| V02 | negative signed delta produces LCN < 0 | reject underflow |
| V03 | run ending beyond volume cluster count | reject external physical reference |
| V04 | sparse-run with b=0 erroneously updates physical base | unit invariant retains previous LCN |
| V05 | nonresident segments overlap/gap with duplicate lowest VCN | report stream corruption |
| I00 | index entry length less than 16 or not aligned | reject/bail safely |
| I01 | index used offset/capacity inconsistent | reject malformed node header |
| I02 | missing terminal entry | reject malformed index |
| I03 | child VCN outside attribute or bitmap clear | reject invalid child |
| I04 | cycle via child VCN/self pointer | terminate with cycle error |
| I05 | key length exceeds entry after tail VCN | reject before name decode |
| I06 | duplicate/inverted sorted keys | report inconsistent collation |
| S00 | security SID subauthority count exceeds blob | reject malformed SID |
| S01 | SD owner/group/ACL offset outside SDS | reject malformed descriptor |
| S02 | ACE/ACL length overflow or 0-length ACE | reject / prevent infinite loop |
| S03 | `$SII` points outside `$SDS`/wrong security ID | report cross-reference mismatch |
| S04 | `$SDH` hash collision with unequal descriptors | compare full SD; never merge by hash alone |
| C00 | compressed token backref outside decoded prefix | reject compressed stream |
| C01 | compressed chunk output exceeds 4 KiB | stop before over-allocation |
| C02 | logical decompression requests gigabytes on tiny malformed input | cap/stream output; fail budget |
| P00 | reparse path points outside extraction root | no-follow / safe representation |
| P01 | names containing slash, `..`, duplicate normalized name | reject or reversible safe encoding |
| P02 | hard-link/reference cycle in malicious directory graph | no recursive loop |
| P03 | MFT mapping references its own invalid byte range | stop bootstrap recursion |
| P04 | log dirty while volume flag claims clean | diagnostic discrepancy; no silent fix |

Model each mutation as an operation on a **valid parsed fixture plus exact field byte offsets**, not random file-wide corruption alone. A spec-derived mutator should know about inter-field constraints such as `used <= capacity` and should deliberately test both one-field failures and mutually consistent but semantically invalid combinations.

### 14.4 Practical fuzzing architecture

Use fast C/C++ libFuzzer targets when the NTFS implementation is C; if fsimg's harness is in another language, keep the byte parser/core behind small stable test adapters. Suggested targets:

1. **`fuzz_ntfs_boot`** — 512–4096-byte input, exercise decode/geometry, no file I/O.
2. **`fuzz_ntfs_fixup`** — record signature, size, USA arrays and trailer restoration; inputs bounded to a small record.
3. **`fuzz_ntfs_attribute`** — resident/nonresident headers, attribute lists, name offsets and record chain traversal.
4. **`fuzz_ntfs_runlist`** — mapping pair parser with randomized starting VCN, 64-bit signed deltas and finite volume bounds.
5. **`fuzz_ntfs_index`** — INDEX_ROOT/INDX node traversal with synthetic child-read callback (bounded graph).
6. **`fuzz_ntfs_unicode`** — 16-bit code units, supplied uppercase table, namespace flags, collision invariants.
7. **`fuzz_ntfs_security`** — SDS header/descriptor/ACL/ACE parsing and index-key validators.
8. **`fuzz_ntfs_lznt1`** (when enabled) — decompressor with fixed maximum output and input budgets.
9. **`fuzz_ntfs_image`** — entire **small** mutated filesystem image in RAM/temporary read-only buffer with resource-limited traversal.
10. **`fuzz_ntfs_roundtrip`** — normalized graph → writer → independent in-project reader, asserting manifest equivalence and allocation invariants.

Run ASan/UBSan, undefined-shift/integer-overflow checks, and fault injection (short reads, premature EOF, seek errors, output ENOSPC). Typical C compiler flags may use `-fsanitize=fuzzer,address,undefined`; details differ by compiler/platform. Seed low-level fuzzers with *freshly synthesized* valid headers and boundary-case fragments, not raw third-party source-code constants or proprietary boot-code blobs.

Use a **small declarative constraints inventory** to drive generation, such as the boot BPB field ranges, alignment constraints, record-size exponent rules, attribute length/offset relations, and runlist signed-nibble semantics. Property-based frameworks and generator scripts (including Ruby where it simplifies orchestration) can produce valid and one-violation variants. No SMT solver is necessary for the first iteration; a handful of typed predicates and mutate-one-field operators covers the highest-value checks.

### 14.5 Cross-implementation differential tests

For each corpus image, normalize each oracle's output into a canonical comparison record:

```text
(path decoded and raw, file identity when available, kind,
 logical data hash per stream, logical size,
 timestamp values, allocated size, flags,
 reported errors and journal/security status)
```

Agree/disagree must be scored at the **semantic** level. Physical MFT record numbers, volume serials, FILE record USA tokens, journal pages, short-name generation and timestamp defaults may legitimately vary across creators. Add categories for "unsupported by oracle" versus "parser disagreement" instead of counting an unsupported feature as an fsimg correctness failure.

Proposed acceptance gates:

- **Parser gate:** zero ASan/UBSan failures; bounded error/timeout behavior on mutated corpus; cross-reader logical hashes match on known-supported fields.
- **Writer gate:** deterministic self-roundtrip; independently readable ordinary files and index/security metadata; no unreferenced allocated clusters; no Windows `chkdsk` corrections on disposable test output.
- **Interop gate:** Windows and Linux writers can independently mutate an fsimg image; fsimg can read the resulting ordinary files, new ADS and directories without losing data.

### 14.6 Test-result provenance

Each test job should store operating-system version/build, implementation version/revision, image SHA-256, geometry, feature flags, whether the source was mounted read-only or read/write, `chkdsk` log, post-test hash, and *pre-write* hash. For Windows, keep the output before repair distinct from after repair. This prevents false success from a writer that happened to rely on an external tool to repair its image.

---
## 15. Recommended implementation milestones and unresolved compatibility gates

### Milestone 0 — fixtures and geometry (short)

Implement volume offset + boot-sector parser + consistent sector/cluster/MFT size computation. Add 512-byte and 4Kn geometry fixtures produced by independent formatters. Provide `fsimg ntfs info`. Deliverable: bad BPBs rejected safely, no extraction yet. Build native fuzz targets early so subsequent structures can be tested independently.

### Milestone 1 — usable NTFS read/extract (highest immediate value)

Implement FILE fixups and header parsing, attributes, ordinary resident/nonresident `$DATA`, mapping pairs, MFT bootstrap, root/small `$I30`, UTF-16 names and `$UpCase`. Add extraction and file hashes. Include timestamps and file flag metadata. A **read-only MVP** can support ordinary Windows-formatted files without needing to understand all metadata internals.

**Do not defer:** checked integer arithmetic, exact error offsets, bounds checks, sequence references and trusted extraction paths. These are core correctness, not optional hardening. Use FreeBSD historical BSD code as the primary candidate for permitted implementation patterns, but examine modern documented behavior independently.

### Milestone 2 — robust real-world reader

Add `$ATTRIBUTE_LIST` and extension MFT records, fragmented MFT, large directory indexes, ADS, hard links, sparse zero-fill, DOS aliases. Build `fsimg ntfs verify` and the corrupt-image corpus. These are high-value prerequisites for extracting arbitrary real Windows volumes, even if the writer won't initially emit them.

This separation is advantageous: real-world NTFS readers must handle complex, fragmented, long-lived metadata, while fsimg's own writer can choose a much simpler, contiguous, read-back-friendly layout.

### Milestone 3 — creation proof-of-concept, **not yet Windows-compatible claim**

Create empty minimal NTFS 3.1 **offline** images with correct BPB, MFT records, mirror, `$Bitmap`, `$AttrDef`, `$UpCase`, root, `$Secure`, `$LogFile`, and volume metadata, plus one small resident file. Implement the layout planner and deterministic serializer. Compare against independent `mkntfs` and Rust formatter output **structurally**, never by copying their proprietary/non-BSD bytes. Reject unsupported overflow and advanced features.

Only when **Windows mounts an untouched image** and `chkdsk` accepts it should this phase be reported as an interoperable formatter. This milestone contains substantial work, especially the security subsystem and journal initialization; do not underestimate it from the modest size of the boot sector.

### Milestone 4 — production P0 writer

Add ordinary nonresident files and nested small directories, deterministic timestamps, correct security inheritance, space accounting and clean Windows writable/remount behavior. Integrate fsimg's library-level create/extract interface. Implement `fsimg verify --strict`, tool-oracle CI and clear failure codes. Initial write profile may reasonably reject volumes that would require directory `$INDEX_ALLOCATION` or extension MFT records.

**P0 release bar:** source-tree → fsimg NTFS image → Windows write on copied VHDX → Windows `chkdsk` clean → independent Linux extraction with matching hashes.

### Milestone 5 — large directories, fragmentation, streams and sparse output

Add index-node generation, security index scaling, attribute extensions when necessary, efficient multi-run files, sparse streams, ADS, and hardlink creation. Avoid in-place editing. Scale to multi-GB test images with fixed small memory and sparse host backing; add directory sizes from 0 to at least tens of thousands of entries.

### Milestone 6 — optional compressed data and reparse features

Implement the MS-XCA-defined LZNT1 decoder for reading and its fuzz suite; consider LZNT1 writer only when a strong user case appears. Reparse points and common symbolic link tags are useful for faithful metadata extraction but require strict security/non-follow defaults. Do not add EFS, WOF, TxF or USN-journal creation without explicit product demand.

### 15.1 Suggested project layout (conceptual)

```text
fsimg/
  src/fs/ntfs/
    ntfs_probe.*       # boot sector, partition/volume geometry
    ntfs_fixup.*       # protected FILE and INDX records
    ntfs_mft.*         # MFT bootstrap and record loading
    ntfs_attr.*        # resident/nonresident attributes, attr lists
    ntfs_runlist.*     # signed-delta mapping pairs and stream I/O
    ntfs_index.*       # I30 and typed non-filename indexes
    ntfs_unicode.*     # UTF-16, volume UpCase and name collation
    ntfs_security.*    # SDS, SDH, SII, SIDs and ACL view
    ntfs_read.*        # object graph / tree traversal / extraction
    ntfs_plan.*        # immutable graph, MFT numbers, extents
    ntfs_write.*       # FILE/INDX serialization, bitmap generation
    ntfs_loginit.*     # validated clean volume journal profile
    ntfs_verify.*      # independent internal cross-reference audit
  tests/ntfs/
    fixtures/           # recipes, manifests, hashes; not copyrighted blobs
    fuzz/               # boot, fixup, attrs, runs, index, security, image
    corrupt/            # specification-derived byte mutations
    oracle/             # Linux/Windows runners and normalization
    test-matrix.*       # profile/feature gates, expected statuses
```

No file layout is mandated by the NTFS format. The goal is making low-level codecs independently testable and preventing format-specific assumptions from contaminating generic fsimg entry/stream APIs.

### 15.2 Explicit issues to resolve by measurement

The following must not be resolved by guessing or cloning another implementation's binary templates:

| Issue | Impact | Required experiment |
| --- | --- | --- |
| Windows-accepted **minimal clean `$LogFile`** initialization | mount/`chkdsk` failure | create bare volumes from multiple independent formatters, compare metadata semantics and Windows results |
| Minimal `$Secure:$SDS/$SII/$SDH` contents and default security descriptors | mount may work, writes/ACLs fail | Windows ACL read/write/create/delete, `chkdsk`, compare security IDs |
| Exact Windows-supported 4Kn/fixup geometry | corrupt FILE/INDX parsing | physical/virtual 4Kn fixtures, verify USA stride and protection |
| Directory index **VCN** unit for index-buffer size smaller than cluster | wrong child lookups | nondefault geometry with large directories |
| `$UpCase` generator version/Windows compatibility | case collisions, bad sorted indexes | independently derived Unicode mapping, compare per-entry lookup and chkdsk |
| `$MFTMirr` mirrored extent length by geometry | boot recovery inconsistency | examine Windows+mkntfs output for several cluster sizes |
| `$BadClus:$Bad` sparse logical layout and MFT attributes | invalid required metadata | profile fixtures with no bad clusters; Windows inspection |
| Exact reserved metadata in `$Extend` under target Windows build | missing optional system features | isolated fresh NTFS Windows formatter fixtures |
| Index parent separator/split semantics and short name collations | large-dir invalidity | generate 100, 1000, 10000 Windows files + mutations |
| Partition-less “superfloppy” NTFS image acceptance on Windows | writer UX / VHDX mounting | explicit raw vs GPT/VHDX test rather than reliance on undocumented mounting |
| Minimal portable security mapping for source POSIX ownership | security semantics | document/default Windows ACL policy and effective-access tests |
| Nonzero/zero LSN and journal clean-volume interaction | false dirty state | Windows remount and exact `chkdsk` diagnostics |

### 15.3 Proposed engineering decision

For fsimg's intended *extract + create* scope, invest first in a **fuller read path and a deliberately narrower offline writer**. Read complexity is proportional to all NTFS layouts Windows may generate; writer complexity can be capped by choosing contiguous file extents, predetermined MFT capacity and resident `$I30` indexes. The immutable planner and typed metadata graph can be reused by later filesystem plugins without reproducing NTFS-specific journal and security designs elsewhere.

Do not implement arbitrary NTFS in-place mutation merely to make creation work: that is a separate, significantly harder project requiring logging, crash consistency, ACL inheritance, allocator updates and index rebalancing.

---

## 16. References and source/license boundaries

**License interpretation for this document:** Source links are *research and validation references*, not incorporated source. Only the clearly identified **BSD-licensed** sources are potential code-adaptation candidates under the user's restriction, subject to examining each file's provenance, original notices and dependencies. MIT/Apache sources are permissively licensed in the usual legal sense, but the user's instruction is stricter: **do not copy their code into fsimg or this document**. GPL/LGPL/CPL/IBM-PL and proprietary sources must not be copied. All structural descriptions in this spec were independently worded.

### 16.1 Published format / official Microsoft references (facts and semantics, no source-code copying)

| Topic | Official documentation | Why it helps |
| --- | --- | --- |
| NTFS MFT | [Microsoft: Master File Table](https://learn.microsoft.com/en-us/windows/win32/devnotes/master-file-table) | record model and file attributes |
| FILE segment | [Microsoft: FILE_RECORD_SEGMENT_HEADER](https://learn.microsoft.com/en-us/windows/win32/devnotes/file-record-segment-header) | core FILE record field semantics |
| Multi-sector fixups | [Microsoft: MULTI_SECTOR_HEADER](https://learn.microsoft.com/en-us/windows/win32/devnotes/multi-sector-header) | USA protection fields |
| Attribute structure | [Microsoft: ATTRIBUTE_RECORD_HEADER](https://learn.microsoft.com/en-us/windows/win32/devnotes/attribute-record-header) | attribute kinds, variants and mapping pairs |
| Attribute list | [Microsoft: ATTRIBUTE_LIST_ENTRY](https://learn.microsoft.com/en-us/windows/win32/devnotes/attribute-list-entry) | extension records and VCN parts |
| Filenames | [Microsoft: FILE_NAME](https://learn.microsoft.com/en-us/windows/win32/devnotes/file-name) | NTFS name payload, namespaces |
| Standard info | [Microsoft: STANDARD_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/devnotes/standard-information) | time/flags/security ID fields |
| Windows FS protocol fields | [Microsoft: MS-FSCC](https://learn.microsoft.com/openspecs/windows_protocols/ms-fscc/) | file system control semantics, names, flags, reparse structures; **not a full disk spec** |
| Self-relative ACLs | [Microsoft: MS-DTYP security descriptor](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-dtyp/7d4dac05-9cef-4563-a058-f108abecce1d) | SD/SID/ACL binary semantics |
| SDDL | [Microsoft: Security Descriptor String Format](https://learn.microsoft.com/en-us/windows/win32/secauthz/security-descriptor-string-format) | fixture security profile and effective ACL validation |
| NTFS streams | [Microsoft: File Streams](https://learn.microsoft.com/en-us/windows/win32/fileio/file-streams) | named ADS and other attribute stream types |
| Reparse behavior | [Microsoft: File System Reparse Points](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/reparse-points) | tag ownership, required filter semantics |
| Reparse structures | [Microsoft: REPARSE_DATA_BUFFER](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_reparse_data_buffer) | junction/symlink payload offsets |
| LZNT1 | [Microsoft: MS-XCA/LZNT1](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-xca/94164d22-2928-4417-876e-d193766c4db6) | compressed-chunk decoding |
| NTFS FSCTL | [Microsoft: FSCTL_GET_NTFS_VOLUME_DATA](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_ntfs_volume_data) | volume inspection APIs |
| NTFS volume info | [Microsoft: fsutil fsinfo](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-fsinfo) | Windows-side geometry/mount oracle |
| Windows format | [Microsoft: format](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/format) | independent production formatter |
| Windows checker | [Microsoft: chkdsk](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk) | structural integrity validation |

### 16.2 Publicly documented on-disk NTFS data structures (format research only)

The **Linux-NTFS/Flatcap NTFS Documentation** is a comprehensive historical technical catalog. It derives from work around Linux NTFS implementations and is **not BSD-licensed code**; its explanations are format references only. The specification here paraphrases observable layout facts without transplanting implementation source.

- [Attribute index and numeric identifiers](https://flatcap.github.io/linux-ntfs/ntfs/attributes/index.html)
- [NTFS boot sector](https://flatcap.github.io/linux-ntfs/ntfs/files/boot.html)
- [MFT record layout](https://flatcap.github.io/linux-ntfs/ntfs/concepts/file_record.html)
- [Generic attribute header](https://flatcap.github.io/linux-ntfs/ntfs/concepts/attribute_header.html)
- [Mapping pairs/data runs](https://flatcap.github.io/linux-ntfs/ntfs/concepts/data_runs.html)
- [FILE_NAME](https://flatcap.github.io/linux-ntfs/ntfs/attributes/file_name.html)
- [STANDARD_INFORMATION](https://flatcap.github.io/linux-ntfs/ntfs/attributes/standard_information.html)
- [ATTRIBUTE_LIST](https://flatcap.github.io/linux-ntfs/ntfs/attributes/attribute_list.html)
- [INDEX_ROOT](https://flatcap.github.io/linux-ntfs/ntfs/attributes/index_root.html)
- [NTFS directories/indexes](https://flatcap.github.io/linux-ntfs/ntfs/concepts/directory.html)
- [NTFS `$MFT` system file](https://flatcap.github.io/linux-ntfs/ntfs/files/mft.html)
- [NTFS `$AttrDef` system file](https://flatcap.github.io/linux-ntfs/ntfs/files/attrdef.html)
- [NTFS `$LogFile` system file](https://flatcap.github.io/linux-ntfs/ntfs/files/logfile.html)
- [NTFS `$Secure` system file](https://flatcap.github.io/linux-ntfs/ntfs/files/secure.html)
- [NTFS `$Volume` information](https://flatcap.github.io/linux-ntfs/ntfs/attributes/volume_information.html)
- [NTFS special system files](https://www.ntfs.com/ntfs-system-files.htm)
- [NTFS shared security and indexes](https://www.ntfs.com/ntfs-permissions-file-structure.htm)
- [Sleuth Kit NTFS implementation notes](https://github.com/sleuthkit/sleuthkit/wiki/NTFS_Implementation_Notes) — *documentation licensed separately; no copying*

### 16.3 BSD-licensed **candidate code** references

| Repository | Confirmed license / caution | Scope / relevant files |
| --- | --- | --- |
| [FreeBSD historical NTFS driver](https://cgit.freebsd.org/src/tree/sys/fs/ntfs?h=stable/14&id=76c1f1f63d3cd8154d54dca52e8ef88e283c179d) | Historical source files use permissive BSD-family two-condition notices; inspect each file | low-level on-disk types, MFT records, runlists, read path, compression routines |
| [FreeBSD historical `ntfs.h`](https://cgit.freebsd.org/src/tree/sys/fs/ntfs/ntfs.h?h=stable/14&id=76c1f1f63d3cd8154d54dca52e8ef88e283c179d) | Explicit BSD-2-style redistribution terms in header; check original NetBSD contributors/notices | older NTFS layout/type definitions |
| [FreeBSD historical `ntfs_subr.c`](https://cgit.freebsd.org/src/tree/sys/fs/ntfs/ntfs_subr.c?h=stable/14&id=76c1f1f63d3cd8154d54dca52e8ef88e283c179d) | verify file header and imported portions | record/attribute/volume helper functions |
| [FreeBSD historical `ntfs_compr.c`](https://cgit.freebsd.org/src/tree/sys/fs/ntfs/ntfs_compr.c?h=stable/14&id=76c1f1f63d3cd8154d54dca52e8ef88e283c179d) | verify file header individually | historical compressed-stream reader |
| [NTFS-Parser-Lib](https://github.com/orkblutt/NTFS-Parser-Lib) | GitHub reports BSD-3-Clause **and an additional LICENSE**; origin is a CodeProject article. **Do not adapt without a separate provenance/license audit of the original author's terms.** | C++ parser/attribute declarations |

These older BSD sources are useful primarily for read-side implementation patterns, not for copying a modern Windows-compatible NTFS **formatter**. As with all licensed source, preserving the appropriate copyright/notice when adapting code is required; the source-code rule here is more stringent than mere linkability.

### 16.4 MIT/Apache **reference implementations** — useful, but **code not to be copied** under this requirement

| Project | License | Why inspect/test |
| --- | --- | --- |
| [Colin Finck `ntfs` (Rust)](https://github.com/ColinFinck/ntfs) | MIT OR Apache-2.0 | actively useful read-only parser, no_std option, resident/nonresident, indexed traversal, ADS; `ntfs-shell` tool |
| [Rust `ntfs` documentation](https://docs.rs/ntfs/latest/ntfs/) | same crate | API surface and high-level feature limitations |
| [`rust-fs-ntfs` (Rust)](https://github.com/christhomas/rust-fs-ntfs) | MIT OR Apache-2.0 | image formatting, read/write, `fsck.ntfs`, C ABI; Windows chkdsk oracle matrix; **not yet 1.0** |
| [`rust-fs-ntfs` alternate repo URL](https://github.com/antimatter-studios/rust-fs-ntfs) | same project / historic ownership; verify redirect | useful when following existing discussions or links |
| [`rust-fs-ntfs` crate docs](https://docs.rs/crate/rust-fs-ntfs/latest) | same crate | exact CLI usage/limitations and build procedures |
| [Go `aoiflux/libntfs`](https://github.com/aoiflux/libntfs) | MIT | feature-rich forensic reader, validation, index types, deleted entries and runlists; **narrow optional byte overwrites are not a general FS writer** |
| [Python `mtivadar/fs_ntfs`](https://github.com/mtivadar/fs_ntfs) | MIT | independent readable Python parser/extractor for MFT and ADS |
| [Python `NTFSparse/ntfs_parse`](https://github.com/NTFSparse/ntfs_parse) | MIT | forensic MFT/LogFile/USN journal correlation and small example image |

**Do not import any of the implementation code in this table into the spec or fsimg** if the requirement is literally to use only BSD-licensed source. They can be executed as independent programs or used to locate independently verifiable bugs and feature gaps.

### 16.5 GPL/LGPL/CPL and other *test-only/reference-only* implementations

| Project | License | Use |
| --- | --- | --- |
| [`ntfs-3g` / `ntfsprogs`](https://github.com/tuxera/ntfs-3g) | GPL-2+ family / check component notices | mature FUSE read/write plus `mkntfs`, `ntfsls`, `ntfscat`, `ntfscp`, `ntfsinfo`, `ntfsfix`, `ntfsresize`, `ntfsclone` |
| [Linux `ntfs3` kernel driver](https://github.com/torvalds/linux/tree/master/fs/ntfs3) | GPL-2.0 | independent kernel read/write compatibility and source for format research **only** |
| [Older Linux NTFS code (`fs/ntfs`)](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/865730eec5435ce40a5dc3c615077d04c8f95098/fs/ntfs/) | GPL family | historical field observations only, not a source for copying algorithms/types |
| [`libyal/libfsntfs`](https://github.com/libyal/libfsntfs) | LGPLv3+ | robust independent read-only parser, NTFS 3.0/3.1, LZNT1 and WOF, `fsntfsinfo`, `fsntfsmount` |
| [The Sleuth Kit](https://github.com/sleuthkit/sleuthkit) | principally IBM Public / Common Public License, with mixed component notices | `fsstat`, `fls`, `istat`, `icat`, deleted records, cluster usage, forensic perspective |
| [QEMU `qemu-img`](https://www.qemu.org/docs/master/tools/qemu-img.html) | software includes GPL components; check installed build | raw disk ↔ VHDX conversion for Windows VM tests; not NTFS formatting |

TSK license nuance: its own project [license overview](https://sleuthkit.org/sleuthkit/licenses.php) says core code uses IBM Public/Common Public licenses, some tools have other licenses, and a standalone GNU `strings` component is GPL. Classify it as **mixed, not BSD** when assessing code reuse.

### 16.6 Oracle tool manuals / compatibility notes

- [`mkntfs` regular-file and geometry options](https://manpages.debian.org/trixie/ntfs-3g/mkntfs.8.en.html)
- [`ntfsinfo` flags](https://manpages.debian.org/unstable/ntfs-3g/ntfsinfo.8.en.html)
- [`ntfsls` listing options](https://linux.die.net/man/8/ntfsls)
- [`ntfscat` file/stream read](https://manpages.debian.org/testing/ntfs-3g/ntfscat.8.en.html)
- [`ntfscp` offline file writing](https://manpages.debian.org/unstable/ntfs-3g/ntfscp.8.en.html)
- [`ntfs-3g` manual, mount and read/write limitations](https://github.com/tuxera/ntfs-3g/wiki/Manual)
- [`ntfs-3g` FAQ, metadata compatibility](https://github.com/tuxera/ntfs-3g/wiki/NTFS-3G-FAQ)
- [`libfsntfs` information program](https://manpages.opensuse.org/Tumbleweed/libfsntfs-tools/fsntfsinfo.1.en.html)
- [`fsntfsmount` FUSE/offset guide](https://github.com/libyal/libfsntfs/wiki/Mounting)
- [Sleuth Kit `icat` examples and sector-offset behavior](https://sleuthkit.org/sleuthkit/man/icat.html)
- [Sleuth Kit command inventory](https://wiki.sleuthkit.org/TSK-Tool-Overview/)
- [PowerShell `Mount-DiskImage`](https://learn.microsoft.com/powershell/module/storage/mount-diskimage)
- [PowerShell `Get-DiskImage`](https://learn.microsoft.com/en-us/powershell/module/storage/get-diskimage)
- [PowerShell `Set-Content -Stream` for ADS](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.management/set-content)
- [Windows NTFS `chkdsk`](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk)
- [Windows `fsutil fsinfo`](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-fsinfo)
- [NTFS USA fixup bounds issue reported May 2026](https://lists.openwall.net/linux-kernel/2026/05/08/1171)

---

## 17. Appendix: implementation quick reference

### 17.1 Magic values and relative address spaces

| Structure | Signature/selector | Address base | Required handling |
| --- | --- | --- | --- |
| NTFS boot sector | OEM `NTFS    ` at `+0x03`; boot sig `55 AA` at `+0x1FE` | volume byte 0 | BPB geometry; last-sector backup |
| MFT record | ASCII `FILE` | record start in `$MFT` logical stream | update-sequence fixups; dynamic attr offset |
| Corrupt MFT record | sometimes ASCII `BAAD` | MFT record start | diagnostic only; do not treat as valid record |
| Index buffer | ASCII `INDX` | logical stream of named `$INDEX_ALLOCATION:$I30` | fixups; child VCN and node-header offsets |
| MFT attr list end | type `0xFFFFFFFF` | within current FILE record | 4-byte sentinel; must be inside used region |
| Runlist end | byte `0x00` | within nonresident attribute runlist | terminates runs; not a zero-length run |
| Root file record | MFT number 5 | `$MFT` logical data | contains `$INDEX_ROOT:$I30` |
| Volume cluster bitmap | `$Bitmap`, record 6 | data value of system record 6 | LCN allocated bit, LSB-first |
| MFT record bitmap | `$BITMAP` on record 0 | attribute value on `$MFT` | MFT record allocated bit, LSB-first |
| `UpCase` | record 10 unnamed DATA | stream content | 65,536 UTF-16LE code units, 128 KiB in standard NTFS 3.x |
| Security descriptors | `$Secure:$SDS` | named DATA stream on record 9 | SDS header+SD; 16-byte aligned; indexes and redundant storage |

### 17.2 Common field width and alignment invariants

- On-disk **integer fields are little-endian** unless explicitly documented otherwise (SID identifier-authority is a notable big-endian field).
- MFT file references are `low 48-bit record number | high 16-bit sequence`.
- MFT attribute records and directory index entries generally align to **8 bytes**.
- Protected FILE and INDX records use USA token + 16-bit replaced sector trailers; **validate the full USA**. Do not assume host physical-sector size is the fixup stride.
- Nonresident mapping pairs use **unsigned** run-length bytes and **signed** relative-LCN bytes, with a delta field width of zero meaning a sparse run.
- A directory's `$INDEX_ROOT` contains a **16-byte index header starting at `value+0x10`**; a nonresident INDX buffer has its index header starting at `buffer+0x18`.
- All `used`, `length`, `capacity`, `entry`, `name` and stream-relative offsets must remain inside their corresponding parent objects. A valid outer image does not excuse nested out-of-bounds fields.
- All created physical clusters are covered by exactly one intended owner in the layout ledger; allocated clusters are marked in `$Bitmap`.
- Every allocated MFT record is marked in `$MFT:$BITMAP`, and every ordinary file/directory index resolves to a non-stale in-use file reference.
- `$Secure` references must resolve through actual SDS/SII/SDH entries; random security IDs are not a security scheme.
- On-disk filenames and NTFS collation are UTF-16 based; host UTF-8 and host casefold algorithms are not authoritative.

### 17.3 Suggested top-level acceptance checklist

```text
[ ] Provenance reviewed: no non-BSD implementation code or proprietary blobs included
[ ] Geometry decoded safely (including nondefault record/index sizes)
[ ] MFT bootstrap and FILE/INDX fixups validated by fuzzing
[ ] Attribute lists/runlists and all supported streams extracted correctly
[ ] Volume-specific uppercase collation used for directory traversal
[ ] ADS, Unicode names and hard links represented losslessly or explicitly limited
[ ] All mandatory metadata files have independently verified valid semantics
[ ] Volume bitmap, MFT bitmap, MFT mirror, index bitmaps and references agree
[ ] Security IDs and the SDS/SII/SDH graph are valid
[ ] Clean logfile/volume state passes Windows validation
[ ] Windows mounts **unrepaired** fsimg image and reads all created content
[ ] Windows can create, rename, delete and remount files on a disposable copy
[ ] Windows chkdsk reports **no repairs required** on new fsimg output
[ ] Independent Linux NTFS-3G, kernel ntfs3 and/or libfsntfs results agree
[ ] Sanitizers and specification-derived corruption suite pass
[ ] Unit and integration tests record exact source revision, image hash, geometry
```

### 17.4 Limits of this specification

This document is intended to be detailed enough to guide implementation of an NTFS 3.x reader and a staged **offline** image generator, but it is not a complete replacement for Microsoft `ntfs.sys`, a crash-consistency proof or a certified NTFS formatter. In particular, exact validated initial `$LogFile` pages, `$Secure` defaults/hashes/index instances, unusual 4Kn/index-VCN encodings, historical NTFS versions and complete 8.3/namespace ordering should be **confirmed with fixtures and Windows `chkdsk`**, not guessed from a small number of reverse-engineered observations. A feature tagged experimental must not be silently advertised as clean Windows-compatible output.

**Bottom line:** fsimg can ship a very useful NTFS reader before tackling a fully Windows-writable formatter. The correct writer architecture is a deterministic, graph-based, two-pass-plus-fixpoint volume builder with a limited, rigorously validated write profile — not a general NTFS mutating filesystem driver.
