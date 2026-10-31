# ISO 9660, Rock Ridge, Joliet, El Torito: filesystem specification and implementation plan for `fsimg`

**Document status:** implementation-oriented research specification / independent engineering design, 20 October 2026.  
**Target project:** private [`stass/fsimg`](https://github.com/stass/fsimg), `master` at inspected tree `204adbe81f0a9dc3a3610e3edd97019439850944`.  
**Primary standard:** [ECMA-119, sixth edition, December 2025](https://ecma-international.org/wp-content/uploads/ECMA-119_6th_edition_december_2025.pdf).  
**Other governing references:** [RRIP 1.12 draft](https://people.freebsd.org/~emaste/rrip112.pdf), [ECMA-119 Annex C (Joliet)](https://ecma-international.org/wp-content/uploads/ECMA-119_6th_edition_december_2025.pdf), and [El Torito v1.0 specification](https://misc.manty.net/eltorito.pdf).  
**License/provenance:** Original descriptive text, independent tables, pseudocode, test plans and design proposals. No code or prose copied from GPL/LGPL/CDDL/proprietary implementations, or verbatim from standards. Published specifications are used as *factual references*, not as text/code to relicense. External copyleft tools are permitted only as **independent test oracles**. In a source implementation, review licenses **file by file**, not only at repository level.

> **Scope and caution.** This is an implementer's guide, not a substitute for the authoritative standards. `MUST` below expresses the intended `fsimg` supported-profile contract unless explicitly called an on-disc standard requirement. Details that are conditional, underspecified by the standard, or known interoperability exceptions are called out separately. The format's legal maximum and `fsimg`'s currently implementable maximum must never be confused.

## Contents

1. Goals, architecture and support matrix
2. Terminology and on-disc addressing
3. ISO 9660 descriptor and numeric representations
4. Primary, supplementary and enhanced descriptors
5. Directory records and tree semantics
6. Path tables
7. Names, versions, namespaces and Unicode
8. Time fields and extended attributes
9. File content: extents, multiple sections and interleaving
10. Rock Ridge and SUSP
11. Joliet
12. El Torito boot catalogs and images
13. Additional extensions and optical-image variants
14. `fsimg` reader driver design
15. `fsimg` writer and deterministic layout design
16. Structural verification, corruption policy and security
17. Specification-driven rules, mutations, fuzzing
18. Interoperability tools and commands (read and write)
19. Conformance and regression matrix
20. Delivery milestones and acceptance gates
21. Primary sources, permissive source code and other references
22. Open design questions / normative resolution checklist

---

## 1. Goals, architecture and support matrix

### 1.1 Product model

`fsimg` creates and extracts filesystem images without mounting them, includes an embeddable C11 library, shares an image reader/writer core, uses bounded scratch-backed metadata, and deliberately does **not** edit existing images in place. Its shared contracts are defined in [`specs/`](https://github.com/stass/fsimg/tree/master/specs), especially [`design-formats.md`](https://github.com/stass/fsimg/blob/master/specs/design-formats.md), [`design-api.md`](https://github.com/stass/fsimg/blob/master/specs/design-api.md), [`design-directories.md`](https://github.com/stass/fsimg/blob/master/specs/design-directories.md), [`design-emission.md`](https://github.com/stass/fsimg/blob/master/specs/design-emission.md), and [`design-spec-testing.md`](https://github.com/stass/fsimg/blob/master/specs/design-spec-testing.md).

Treat ISO 9660 as a *format descriptor with one reader and one writer*, using named profiles and descriptor-specific **hierarchy selections**. Avoid building a second directory walker or an optical-specific public API. A disc can hold several distinct filesystem name trees pointing to shared file data:

- **Primary ISO hierarchy** (PVD), ASCII-ish d-character identifiers, numeric versions.
- **Rock Ridge hierarchy**: logically a POSIX interpretation of the **primary** hierarchy using SUSP/RRIP records; includes symlinks, owners, modes and relocations.
- **Joliet hierarchy**: a *separate* SVD directory tree with UCS-2BE identifiers; metadata does not come from the primary tree by matching names.
- **Enhanced hierarchy**: a separate type-2, version-2 hierarchy with different name rules, not equivalent to Joliet.

A selected directory tree and its naming rules form an atomic read view. Never mix directory records or resolve a child in another hierarchy by matching a path spelling.

### 1.2 Feature contract

| Feature | Reader target | Writer target | Priority |
|---|---|---|---|
| ISO base PVD and Level 1/2 | Complete | Complete | P0 |
| Single ordinary file extent, 2048-byte cooked image | Complete | Complete | P0 |
| Type L / M path tables | Parse/validate | Emit both | P0 |
| Numeric versions, hidden/associated flags | Preserve semantics | Deterministic `;1` | P0 |
| Joliet levels 1–3, UCS-2BE | Read/verify | Emit selected level | P1 |
| RRIP / SUSP (NM/PX/TF/SL/CE) | Read/verify | Emit | P1 |
| RRIP CL/PL/RE relocation | Read/verify | Optional; may cap primary depth | P1/P2 |
| Large files via multi-extent | Read | Emit | P1/P2 |
| El Torito boot record/catalog | Inspect/verify | Emit from explicit boot-image source | P2 |
| Enhanced (type 2, version 2) | Reader | Later | P2 |
| XAR, interleaving, associated/record files | Structural and content reader where safe | Later | P2 |
| Multiple volume-set members, multisession | Detect/explicit session choice | Later | P3 |
| CD-ROM XA, raw 2352/2336, BIN/CUE | Recognize incompatibility | Later transport adapter | P3 |
| zisofs, AAIP, Apple extensions | Identify/return unsupported or preserve isolated hints | Later separate features | P3 |
| UDF and UDF/ISO hybrid | Read the ISO hierarchy if valid | Separate UDF driver | Separate |

**Proposed portable default writer:** 2048-byte sectors, ISO Level 2 primary tree + Rock Ridge + Joliet (where enabled), ordinary no-interleave extents, `;1` identifiers, a single session, source content not compressed, no XAR, one PVD + optional SVD + terminator, and no boot catalog unless specifically requested. This provides a plain ISO view while exposing useful names and POSIX metadata on modern systems. **If strict legacy interchange Level 1 is requested**, enforce 8.3 / 8-character names, restricted directory depth and single-section files.

### 1.3 API boundary and supported features

The existing `fsimg_format_t` descriptor declares `probe_bytes`, a pure `probe(prefix, length, volume_length)`, separately optional `reader` and `writer`, and public `fsimg_format_variant_t` capability records. The ISO signature lives **32 KiB into the image**; the first descriptor may be a boot record. Therefore a PVD-at-exactly-sector-16 probe is wrong. Either declare a finite descriptor-prefix bound (recommend scan sectors 16–79 = 128 KiB for discovery, with a larger explicit-profile bound if needed) or extend the common probing contract to allow **a second, bounded probe read**. Do not silently mutate the common auto-probe model. A valid matching descriptor set beyond the chosen bound is `FSIMG_EUNSUPPORTED`, not an unsafe unlimited scan.

`fsimg_read_options_t` / `fsimg_write_options_t` do not presently have generic ISO extension selectors. Do **not** repurpose `fat_codepage`, generic `sector_bytes` as a fake Joliet level, or reserved struct fields. Propose versioned typed ISO-specific option records with a small documented core; alternately publish explicit names `iso9660`, `iso9660-rr`, `iso9660-joliet`, etc., as format variants *with the same shared descriptor*, noting that formats select at reader open as well as writer planning. Avoid proliferating combinations in public names. The driver must register in [`lib/formats/format.c`](https://github.com/stass/fsimg/blob/master/lib/formats/format.c) and expose capability records truthfully.

## 2. Terminology and on-disc addressing

### 2.1 Units and file kinds

| Term | Definition / implementation note |
|---|---|
| Logical sector | In the normal **cooked** ISO image, 2,048 bytes. Offset of LBA `n` is `n * 2048` (checked). |
| Logical block | Addressing unit in the ISO volume, reported by descriptor's 16-bit-both-endian logical-block-size field. Writer uses 2,048; reader explicitly limits supported logical-block geometries and must not conflate logical sectors and logical blocks when they differ. |
| LBA / RBA | Logical block/sector address as defined by a record. Some El Torito documents call sector addresses *RBA*; do not add the boot catalog's LBA twice. |
| System area | First 16 logical sectors, 0–15 (32,768 bytes), typically zero-filled; may host hybrid MBR/GPT/system-specific data, so a reader must never reject an otherwise valid image merely because it is nonzero. |
| Data area | Subsequent sectors holding descriptors, path tables, directories, files, continuation areas and possibly boot data. |
| Extent | Contiguous address range allocated to a file section/directory, possibly preceded by an extended attribute record. |
| Directory record | Variable-length item describing a file section, subdirectory, special current/parent record, and optional system-use metadata. |
| PVD | Primary Volume Descriptor (type 1). |
| SVD | Supplementary Volume Descriptor (type 2, version 1); Joliet is a particular SVD signature/encoding. |
| EVD | Enhanced Volume Descriptor (type 2, version 2); **different from Joliet**. |
| SUSP | System Use Sharing Protocol, a tagged system-use framing/continuation convention. |
| RRIP | Rock Ridge Interchange Protocol, a POSIX metadata namespace on ISO. |
| ISO namespace vs payload | A physical file extent may be referenced by ISO, RR, Joliet, or other hierarchies. Multiple legal references **are not automatically corruption**. |

### 2.2 Addressing arithmetic

Use `uint64_t` for intermediate offsets and sizes, with checks before multiply and add. A 32-bit block LBA at 2048 bytes can reference near 8 TiB of address space; individual file-section length is 32-bit (under 4 GiB), but a multi-section file can be larger. **Never** truncate `file_size`, path-table sizes, or sums to a 32-bit host type. `fsimg` supported image limit may be narrower for practicality.

Standard reader formula for ordinary file data when an extended attribute record precedes the extent:

```
extent_start_byte = checked_mul(extent_lba, logical_block_bytes)
data_start_byte   = checked_add(extent_start_byte,
                                checked_mul(xar_blocks, logical_block_bytes))
logical_data_size = file_section_data_length   // XAR excluded
required_blocks   = ceil_div(logical_data_size, logical_block_bytes)
```

These positions refer to **volume-relative logical block numbers**. If supporting sessions later, retain separate `session_vds_start_lba` and volume-addressing base; a later-session descriptor pointer does not automatically mean all data locations are session-relative. For a raw-image slice, use the already bounded `fsimg_io_slice` and *only* a documented explicit addressing transform. Validate every `[offset, offset+length)` before reading; zero-length content is legitimate.

### 2.3 Raw optical transport is a separate layer

A `.iso` normally contains 2048 data bytes per sector. Physical CD mode 1 sectors can be 2352 bytes, with headers/ECC; CD-ROM XA Mode 2 Form 1 provides 2048 user-data bytes, while Form 2 commonly provides 2324. BIN/CUE, NRG, MDF/MDS, compressed CSO/ZSO, CDDA tracks and TOC gaps are **not** interchangeable raw `.iso` byte arrays. `fsimg` should reject unsupported raw encodings or introduce a separate `fsimg_io_t` logical-sector adapter, not infer 2048-byte extraction by searching for `CD001` inside raw sectors.

## 3. ISO 9660 descriptor and numeric representations

### 3.1 Numeric encodings

| Designator | On-disk length | Meaning |
|---|---:|---|
| `711` | 1 | Unsigned 8-bit |
| `712` | 1 | Signed 8-bit (used in timezone) |
| `721` / `722` | 2 | Unsigned 16-bit LE / BE |
| `723` | 4 | A 16-bit value recorded once LE and again BE; **both must agree** |
| `731` / `732` | 4 | Unsigned 32-bit LE / BE |
| `733` | 8 | A 32-bit value recorded LE and BE; **both must agree** |

These names are conventional shorthand; parsers should name concrete helpers (`iso_733`, `iso_723`) and decode by byte load and shifts, not unaligned host structs. The original specification has no global checksum for primary descriptor metadata: duplicated endian fields are an essential consistency check.

### 3.2 Volume descriptor sequence

Volume descriptors occupy logical sector-size slots, starting at sector 16 of the chosen session. A descriptor begins with:

| Byte offset | Size | Field | Typical value |
|---:|---:|---|---|
| 0 | 1 | Descriptor type | 0, 1, 2, 3 or 255 |
| 1 | 5 | Standard ID | ASCII `CD001` |
| 6 | 1 | Descriptor version | 1 for normal descriptors; 2 for EVD's descriptor version |
| 7–2047 | 2041 | Type-dependent payload | — |

Common types: **0** boot record; **1** PVD; **2** SVD/EVD; **3** volume partition descriptor; **255** set terminator. Reserved or unknown type codes are handled conservatively. For a type-255 descriptor, the remainder is expected to be zero in the standard profile. The first descriptor can be type 0, not necessarily type 1. A valid volume descriptor set includes its obligatory PVD and terminal descriptor; accept the ordering defined by the standard without assuming a particular optional descriptor sequence.

**Probe and open policy:** inspect at most the declared descriptor window; verify descriptor signature `CD001` and version for each claimed descriptor; distinguish absent signature (`FSIMG_EFORMAT`), recognized but unsupported type/geometry/extension (`FSIMG_EUNSUPPORTED`) and malformed in-scope descriptors (`FSIMG_ECORRUPT`). Do not guess a format because six stray bytes somewhere in a short file resemble a PVD. Open rescans and validates the selected set independently of probe.

### 3.3 Primary volume descriptor (PVD): exact zero-based offsets

The table below is an independently assembled byte map of the 2048-byte PVD. `723` and `733` are the numeric encodings above. All multi-byte spans are inclusive.

| Offset | Length | Contents | Encoding |
|---:|---:|---|---|
| 0 | 1 | Type | 1 |
| 1 | 5 | Standard identifier | `CD001` |
| 6 | 1 | Descriptor version | 1 |
| 7 | 1 | Unused | 0 |
| 8–39 | 32 | System identifier | a-characters, padded with spaces |
| 40–71 | 32 | Volume identifier/label | d-characters, space padded |
| 72–79 | 8 | Unused | 0 |
| 80–87 | 8 | Volume space size, in logical blocks | `733` |
| 88–119 | 32 | Unused | 0 |
| 120–123 | 4 | Volume set size | `723` |
| 124–127 | 4 | Volume sequence number | `723` |
| 128–131 | 4 | Logical block size | `723` |
| 132–139 | 8 | Path-table byte count | `733` |
| 140–143 | 4 | Type-L path table address | `731` |
| 144–147 | 4 | Optional second type-L address | `731`; zero if absent |
| 148–151 | 4 | Type-M path table address | `732` |
| 152–155 | 4 | Optional second type-M address | `732`; zero if absent |
| 156–189 | 34 | Root directory record | 34-byte directory-record encoding |
| 190–317 | 128 | Volume-set identifier | string |
| 318–445 | 128 | Publisher identifier | string |
| 446–573 | 128 | Data-preparer identifier | string |
| 574–701 | 128 | Application identifier | string |
| 702–738 | 37 | Copyright file identifier | string |
| 739–775 | 37 | Abstract file identifier | string |
| 776–812 | 37 | Bibliographic file identifier | string |
| 813–829 | 17 | Creation timestamp | 17-byte timestamp |
| 830–846 | 17 | Modification timestamp | 17-byte timestamp |
| 847–863 | 17 | Expiration timestamp | 17-byte timestamp |
| 864–880 | 17 | Effective timestamp | 17-byte timestamp |
| 881 | 1 | File structure version | 1 |
| 882 | 1 | Reserved | 0 |
| 883–1394 | 512 | Application use | opaque |
| 1395–2047 | 653 | Reserved | 0 |

Root record's identifier is the special single zero byte and its directory flag is set. `volume_space_size * logical_block_bytes` is the logical filesystem bound, not necessarily the entire backing file size (trailing pad or appended session structures may exist). Do not require that every physically present image byte be referenced, nor automatically make a volume set's other members available.

### 3.4 Supplementary and enhanced descriptors

A type-2 descriptor repeats most PVD offsets, with key differences:

- Offset 6 equals **1** for supplementary, **2** for enhanced.
- Offset 7 is **volume flags** for type-2 (not the PVD unused field).
- Offsets **88–119** hold **32 bytes of escape-sequence information** for a supplementary descriptor; this is where Joliet is identified.
- Root record, path tables, string fields and name encoding apply to the **specific hierarchy** selected by this descriptor; don't read the PVD path tables when walking an SVD root.
- EVD uses separately agreed character semantics and larger identifiers; `2` alone does **not** identify Joliet.

A volume can contain multiple type-2 descriptors. Selection requires deterministic documented precedence, or an explicit user selector when candidates conflict. Unsupported character sets may coexist with a valid PVD: report them and use the supported PVD rather than rejecting the entire image unless a forced mode requires one of them.

### 3.5 Volume partition descriptor

Type 3 is a standards-defined metadata descriptor, **not** a GPT or MBR partition and not evidence that it is safe to skip arbitrary bounds. In its conventional layout, offset 7 is unused, 8–39 system identifier, 40–71 partition identifier, 72–79 a `733` location and 80–87 a `733` length in logical blocks, with the remaining bytes system-use. Recognize and bounds-check it, but do not claim to interpret its system-specific bytes or mount another namespace from it. The basic file reader doesn't need partition support to follow a valid PVD.

## 4. Descriptor-set validation and profile constraints

At open, build a *descriptor inventory* (type, version, sector offset, characteristic extensions, root extent/length, path table addresses, volume-set information). Select the one effective hierarchy only after the inventory is validated. Repeated or duplicate PVDs are possible in the interchange format; don't treat repeated primary descriptors as a reason to silently select the last. The selected PVD and SVD/EVD must have compatible volume geometry and set identifiers where standards require them. Unknown supplementary descriptors are not fatal to the primary hierarchy. Conflicting duplicate descriptors must be classified explicitly (strict verification can reject; compatibility may select an unambiguous first descriptor and report the conflict).

**Base-profile validation:** `volume_space_size > 0`; nonzero valid logical block size; checked data area; volume sequence and set count valid; root record length and flag valid; path tables in bounds; descriptor chain terminates within finite configured work budget; every pointer describes a range within the *declared* volume as well as the supplied backing image. The scanner must not read arbitrary blocks outside its prefix. Structural verification subsequently validates all referenced metadata, rather than eagerly reading everything during `open()`.

**Interchange level is not recorded as a trustworthy single-byte on-disc marker.** Infer behavior from actual identifiers, hierarchy geometry, multiple sections and explicit writer mode. A descriptor saying `CD001` is not proof of conformance at any level. See ECMA-119 clauses 11.2–11.4 and Annex C for level distinctions. Standard primary-tree Level 1 needs 8.3 names and one section; Level 2 relaxes names but still single-section; Level 3 permits multiple file sections, not arbitrary violation of all other rules. Joliet's three *UCS-2 character-set* levels (`%/@`, `%/C`, `%/E`) are a different dimension, not ISO interchange levels.

## 5. Directory records and tree semantics

### 5.1 Exact directory record format

All offsets below are **relative to the start of a record** and zero-based. No two-byte or four-byte field can cross the record's reported end.

| Offset | Bytes | Field | Encoding / behavior |
|---:|---:|---|---|
| 0 | 1 | `LEN_DR` | Actual record length, 1–255 in storage; **zero at a sector boundary remainder means unused padding, not an entry**. Standard encoded record length must be even and fit sector. |
| 1 | 1 | Extended attribute record length | Number of logical blocks reserved for XAR (normally zero). |
| 2–9 | 8 | Location of extent | `733` block number |
| 10–17 | 8 | File-section data length | `733` byte count (excludes XAR) |
| 18–24 | 7 | Recording timestamp | Section 8 |
| 25 | 1 | File flags | Section 5.2 |
| 26 | 1 | File unit size | Logical blocks per interleave unit; 0 for ordinary content |
| 27 | 1 | Interleave gap | Logical blocks between units; 0 for ordinary content |
| 28–31 | 4 | Volume sequence number | `723` |
| 32 | 1 | File identifier length | Byte count `N`, not character count |
| 33…`32+N` | `N` | File/directory identifier | Encoded by selected hierarchy |
| `33+N` | 0 or 1 | Padding | One `0x00` byte if `N` is even; absent if `N` odd |
| Remainder | variable | System use area (SUA) | Opaque ISO base; SUSP parsing only if established; optional terminating `0x00` to even length |

Formula: `fixed_header=33`, `min_record=33+N+(N%2==0 ? 1 : 0)`; the optional system-use bytes and final even-length pad must still satisfy the record length and sector boundary. The PVD root directory record has `LEN_DR=34`, `N=1`, name `0x00`, and directory flag.

**Framing loop:** for directory-byte offset `p < directory_data_length`, let `sector_remainder = 2048 - ((data_start+p) % 2048)` (when reader supports block-size variants, apply correct sector/block conventions). If the first byte is zero, require remaining bytes *to the end of that sector within the directory extent* to be zero, and continue from the next sector. Otherwise require `LEN_DR >= 34` for a standard entry and `LEN_DR <= sector_remainder`; parse checked slices; reject a record that crosses any sector boundary. Every directory data length and extent must be within the volume. All directory content is described as a run of records padded with zeros to logical-sector boundaries; do not use NUL-terminated strings on disk.

**Subtlety: record length 255 is representable in the field but cannot be a valid even-length ISO directory record.** Standard maximum commonly quoted is 254. System-use entries have their own 8-bit length ceiling, independent of `LEN_DR`.

### 5.2 Flags (byte 25)

| Bit mask | ISO meaning | `fsimg` behavior |
|---:|---|---|
| `0x01` | Existence/hidden | May omit from ordinary display; structural verify still examines it; preserve hidden metadata when supported. Never use hidden as a license to skip safety checks. |
| `0x02` | Directory | Directory records describe child directory extents (not regular file content). |
| `0x04` | Associated file | Non-standard relationship with similarly identified normal file. Keep separate identity; do not flatten into primary file without a public representation policy. |
| `0x08` | Record-format flag | Requires interpretation of compatible XAR; unsupported specialized record semantics must be surfaced. |
| `0x10` | Protection / XAR owner+permissions | Don't fabricate POSIX access bits from it; see XAR. |
| `0x20`, `0x40` | Reserved | Strict verifier rejects nonzero. |
| `0x80` | More file extents follow | Assemble consecutive sections with same file identifier until terminal record. |

A directory must not carry multi-extent bit; bits 2, 3 and 7 are disallowed there. Without XAR, record-format/protection bits must be zero. **Associated files and versions can produce duplicate apparent names** if a reader strips `;N`: namespace design must account for collisions instead of accepting whichever record appears first.

### 5.3 Current and parent records

Every directory begins with reserved special identifiers: byte `0x00` (self) and `0x01` (parent). Their lengths are **one byte even under Joliet**. They are internal directory structure, not enumerated `.` and `..` file entries. Verify appropriate pointers, flags and parent relationships; root parent refers to root. RRIP `NM CURRENT` / `NM PARENT` metadata attached to those entries does not change this identity.

Directory hardlink/cycle prevention differs from file hardlinks: the *logical hierarchy* must remain acyclic. When encountering a repeated directory extent, first distinguish the self/parent records and legitimate RRIP CL relocation pointer from a distinct directory claiming the same storage. A naive global `seen LBA` bitset produces false positives in relocated RR trees and separate Joliet trees; key claims by `{hierarchy, semantic directory identity / resolved relocation}` and validate link meaning before declaring a cycle.

### 5.4 Directory ordering

The media requires a specified sort order for directory entries and path tables; the base ordering uses identifier fields (file base, extension, version, associated flag and section ordering), **not** RRIP `NM` names. Writers MUST sort ISO identifiers independently from source UTF-8 names and from Joliet UCS-2BE sort keys. Joliet sorts coded 16-bit codepoints in big-endian byte order (not locale-specific linguistic collation). RRIP names are logically a lookup view and need not be sorted by their displayed spellings. For acceptance, add `ISO.DIR.SORT` and `ISO.PATH.SORT` structural rules and check them on images generated by `fsimg`; where real-world readable discs are non-conforming, optionally distinguish diagnostic compatibility mode from strict `verify()`.

### 5.5 Directory ownership and record metadata

A directory entry gives `extent_lba` and `data_length` for the child directory; its `.` record inside the child should point to the corresponding directory data, including any recognized relocation semantics. Directory lengths are stored as 32-bit **byte lengths** and can span multiple logical blocks. Avoid assuming a directory is a single 2048-byte sector or that all of its names fit in RAM. Common prepared-directory code already charges entries, materializes bounded replay records and external-sorts keys: use it.

For an ISO record, the private replay payload should carry only the minimum canonical, endian-decoded facts necessary for revalidation (descriptor/hierarchy identity, record location, flags, XAR blocks, start LBA, data size, multi-extent-group locator, SUSP locator, optional file identity). Never store raw source pointers or native-padded C structs in scratch data. Re-validate backing image and scratch framing before replay.

## 6. Path tables

### 6.1 Record layout

Type L uses little-endian fields; Type M uses big-endian fields. The fixed header is eight bytes:

| Offset | Size | Contents |
|---:|---:|---|
| 0 | 1 | Directory identifier byte length `N` |
| 1 | 1 | Extended attribute record length |
| 2–5 | 4 | Directory extent address, LE (L) / BE (M) |
| 6–7 | 2 | Parent path-table **record number**, LE (L) / BE (M) |
| 8… | `N` | Directory identifier (root = single `0x00`) |
| after name | 0 or 1 | `0x00` pad if `N` odd |

Record bytes = `8 + N + (N & 1)`. Parent indices start at **1**, root is record number 1 (and itself its parent). The path table has no timestamps, data lengths, RRIP names or SUSP entries. Both mandatory L and M copies describe the same logical tree; optional second occurrences should agree when present. The L/M tables are **not** bytewise identical because their integer byte order differs.

### 6.2 Ordering, consistency and limits

The path table is hierarchical: parents precede descendants; order by hierarchy depth and defined identifier ordering among siblings. Check parent `< child_record_number` for non-root, no missing ancestor, proper root identity, uniqueness within each parent's namespace, and consistency against the selected directory tree. There is a practical 16-bit bound from parent record numbers; a writer should cap the number of directory path-table records by a documented supported count rather than create an image whose parent index wraps. Directory **count** and path-table byte count are independent limits.

The PVD path tables are the PVD hierarchy. A Joliet SVD has its **own L and M path tables** using UCS-2BE names. Do not cross-match path-table directory names between primary and supplementary hierarchies. An RRIP relocated directory may intentionally create non-obvious relationships relative to its ISO physical path table: validate ISO physical path table using ISO physical topology and RRIP semantic tree using CL/PL/RE separately.

**Writer plan:** assign stable directory numbers in breadth-first/depth order, emit both endian variants, pad every complete path-table *area* to a full block but set descriptor `path_table_size` to the actual unpadded byte count, and optionally emit redundant copies for robustness. Use `uint64_t` scratch-backed counts; error clearly if anything exceeds 32-bit path-table bytes or the 16-bit parent-number limit.

## 7. Names, versions, namespaces and Unicode

### 7.1 Primary identifiers

Primary `d` characters are the restricted uppercase ASCII set `A–Z`, `0–9`, underscore; file separator `.` and version separator `;` are structural. Directory names have no filename version. A file identifier has a base, a period separator, an extension (which may be empty in legal forms), semicolon and decimal version 1–32767. `README.TXT;1` and `README.;1` are typical examples. A version is part of the **on-disc identity**, not a host path component that can be discarded without a collision policy.

Levels:

| Primary interchange | Writer conditions |
|---|---|
| Level 1 | At most 8 primary name characters and 3 extension characters, directory ID at most 8; single file section. |
| Level 2 | File base + extension characters at most **30** excluding separator and version digits; directory identifier at most **31** bytes; single section. |
| Level 3 | Same ordinary name rules as Level 2; multiple file sections allowed in conforming ordering. |

In a normal primary tree, depth is at most 8 levels **including root at level 1**. Under ECMA's modern enhanced type-2 descriptor, name limits and hierarchy rules change; do not generalize those rules to PVD. Standard maximum path constraint is the sum of the file identifier, all relevant directory identifier lengths and separator count, at most 255 for the ordinary hierarchy. Joliet Annex C relaxes this to its own 240-byte-style path accounting, and RRIP can construct deeper *logical* trees via relocation.

**Name derivation on write** must be deterministic and collision-free within *every* emitted hierarchy. Proposal:

1. UTF-8 source name → strict validation (no NUL, slash, unsafe path component), optional chosen Unicode normalization policy *without relying on host locale*.
2. For ISO projection: produce uppercase d-character approximation (nonrepresentable characters converted via fixed escape/substitution or hash-backed aliases); preserve a defensible readable prefix and stable suffix if truncated.
3. Append extension and `;1` for files; ensure projected filename and directory limits.
4. Resolve collisions *globally within each directory*, in canonical source order, by deterministic suffix derivation or a stable digest, **not** by first directory iteration order.
5. For Rock Ridge, keep the actual source UTF-8 path via `NM` after validating bounded byte lengths and namespace uniqueness.
6. For Joliet, encode representable characters directly as UCS-2BE and reject/fallback deterministically for codepoints outside BMP or illegal names; do not silently replace with `'?'` producing collisions.

A byte-for-byte reproducible image should not depend on `readdir` order, source filesystem case-folding, current timezone, collating locale or catalog insertion order. `fsimg`'s existing catalog/external sorting infrastructure is appropriate.

### 7.2 Reader name-resolution policy

Proposal for default `auto` hierarchy selection: **Rock Ridge > Joliet > enhanced (if supported) > primary**, *only when the selected hierarchy is positively identified and internally valid*. A forced choice must not fall back silently. For strict header corruption within an explicitly selected hierarchy, return corruption; unsupported optional names should not poison base PVD recognition. A reproducible `fsimg ls` should expose exactly one selected hierarchy at a time; `--namespace=iso|rr|joliet|enhanced|auto` is a proposed CLI flag, not currently implemented.

**Versions:** default logical display may remove `;1` for the common single-version case, but preserve `;N` when multiple versions would otherwise collide. Better: pre-scan directory, group by canonical base and make display names unambiguous; retain an explicit `raw_iso_identifier` in private state for lookup. Query `NAME.EXT;2` resolves version 2; query without version follows a documented preference (prefer highest version **only if** requested) or is ambiguous and rejected; never do accidental first-hit lookup. A straightforward first implementation preserves versions as `;N` unless there is exactly one visible version, avoiding lossy conflation. Preserve read-only associated-file identity similarly.

`fsimg` uses opaque namespace keys. ISO primary keys should derive from *the selected displayed name policy* (not per-call locale `toupper`); Joliet requires exact decoded UCS-2/UTF-8 matching unless a documented case-folding mode is implemented. Some hosts interpret Joliet names case-insensitively; **do not** claim that locale-insensitive casefold matches Unicode fully. RRIP POSIX names use exact byte-sensitive matching. Reject ambiguity during directory preparation using the common `keys` / `key` callbacks.

### 7.3 Reserved and lossy names

Test at least names `A.TXT`, `A.TXT;1`, `A.TXT;2`, `.`, `..`, empty basename/extension legal edge, embedded delimiters, repeated `.` in a host name, mixed case, leading dot, names differing only by case, long prefix collision, composed/decomposed accent, emoji outside UCS-2, non-ASCII BMP, and names that are legal on Unix but forbidden in Joliet (e.g. `:`, `?`, `\`). The writer must either encode them through a lossless RRIP path plus a deterministic restricted ISO alias, or return a specific error if the requested hierarchy cannot represent them. Never emit a Joliet name containing raw UTF-8 bytes as if UCS-2.

## 8. Time fields and extended attributes

### 8.1 Seven-byte recording date

Directory record offset 18 is seven bytes:

| Byte | Meaning | Range |
|---:|---|---|
| 0 | Years since 1900 | 0–255 |
| 1 | Month | 1–12 |
| 2 | Day | Calendar-valid |
| 3 | Hour | 0–23 |
| 4 | Minute | 0–59 |
| 5 | Second | 0–59 |
| 6 | Signed quarter-hour timezone offset | -48…+52; signed two's-complement |

An all-zero seven-byte date means unspecified. Validate leap days, timezone range and representability before Unix-time conversion. Actual timestamp meaning is *time extent information was recorded*, not necessarily POSIX `mtime`. The writer should use a declared policy (`source modified time` mapped to recording time if fidelity is accepted; otherwise generation epoch for reproducibility) and should report distinctions in metadata-loss accounting.

### 8.2 Seventeen-byte descriptor and RRIP time representations

PVD timestamps: 16 ASCII digits `YYYYMMDDHHMMSScc` (centiseconds), followed by an 8-bit signed GMT quarter-hour offset; an all-zero digit field means unset under corresponding rules. Validate that every decimal field is numeric, timezone valid, and no impossible calendar date or out-of-range year is silently normalized. RRIP `TF` contains multiple 7-byte **short** or 17-byte **long** timestamps, in an explicit flag-selected sequence (see Section 10.5).

The public [`fsimg_timestamp_t`](https://github.com/stass/fsimg/blob/master/include/fsimg.h) preserves `seconds`, `nanoseconds`, `precision_ns`, `basis` and an offset in minutes. `FSIMG_TIME_OFFSET` is useful for on-disc offsets divisible by 15; when on-disc time has no reliable timezone, use `FSIMG_TIME_WALL` rather than inventing UTC. ISO seven-byte time has 1-second precision; 17-byte time has centisecond precision. RRIP `TF` should supply creation/modified/access/change when actually present, with the distinct `valid` bits; timestamps not encoded must not appear as valid. A caller marking unsupported fields as `required` causes planning to fail instead of silently discarding them.

### 8.3 Extended Attribute Record (XAR)

The optional XAR lies in one or more whole logical blocks *at the beginning of the referenced extent*, before file-section content. Its size is given by the directory record's byte-1 block count. First 250 bytes of the XAR define core metadata; 0-based record table:

| Offset | Length | Field |
|---:|---:|---|
| 0–3 | 4 | Owner identification (`723`, zero = not specified) |
| 4–7 | 4 | Group identification (`723`) |
| 8–9 | 2 | Permissions bitmap (not POSIX mode) |
| 10–26 | 17 | Creation timestamp |
| 27–43 | 17 | Modification timestamp |
| 44–60 | 17 | Expiration timestamp |
| 61–77 | 17 | Effective timestamp |
| 78 | 1 | Record format |
| 79 | 1 | Record attributes |
| 80–83 | 4 | Record length (`723`) |
| 84–115 | 32 | System identifier |
| 116–179 | 64 | System use |
| 180 | 1 | XAR version (normally 1) |
| 181 | 1 | Length of escape sequences |
| 182–245 | 64 | Reserved zero |
| 246–249 | 4 | Application-use byte count (`723`: both-endian 16-bit value) |
| 250… | variable | Application-use bytes, then escape sequences |

XAR owner/group values and permissions do not imply Unix `st_mode`. If complete support is deferred, a reader must still correctly **skip** XAR blocks and validate total extent bounds before reading ordinary payload, and must not report protected/record-formatted content as if fully interpreted. For XAR records with fixed/variable record structure, normal byte-for-byte extraction of data stream can be possible, but the application-level record interpretation should be recorded as unsupported/lossy, not guessed. Use `FSIMG_META_OWNER`, `FSIMG_META_CREATED`, `FSIMG_META_MODIFIED` as valid only when independently trustworthy from XAR or RRIP.

## 9. File content: extents, multiple sections and interleaving

### 9.1 Ordinary file sections

Each directory record describes one logical file section (`data_length <= 0xffffffff`). The default writer uses `xar_blocks=0`, `file_unit_size=0`, `interleave_gap=0` and contiguous padded allocation. `logical_file_offset → physical_extent` uses the descriptor's `start_lba` and a checked byte displacement. For each file, `fsimg_reader_resolve()` must provide the **largest safe single extent containing the queried logical offset**, constrained by file logical length and the `content_domain`; the common reader checks the returned segment.

**Storage ownership:** ISO directory, boot catalog, SUSP CE and file data extents may legitimately share physical sectors in a *single intentionally shared payload* (ISO/Joliet duplicate references, RRIP hardlinks). Do not use a single global cluster-owner bitset and call all duplicate references corruption. Verify at the semantic object level, with a canonical data extent list and allowed alias map, while rejecting **inconsistent overlapping payload** and metadata/data overlaps not expressly permitted. Empty files may use benign location conventions; do not demand a nonempty extent allocation.

### 9.2 Multiple extents (Level 3)

A large file can be represented as a consecutive **run of directory records with the same file identifier**. Bit 7 (`0x80`) signals a following section. The final section has that bit unset. For a group:

- Records must have compatible file identity, owner hierarchy, sequence, file attributes, and expected ordering.
- Every section's LBA, XAR, interleaving geometry and length is checked independently.
- The logical file size is a **checked 64-bit sum** of section data lengths.
- Reject orphan continuation, mismatched name, zero/overflowed impossible continuation, incomplete end and group interleaving with another file.
- Do not emit one `fsimg_entry_t` per section; emit **one logical file**, whose `resolve` spans section transitions.

**Writer target:** partition files requiring more than a 32-bit section length into block-aligned intermediate sections (commonly boundary `<= UINT32_MAX` rounded down to 2048) plus final tail. Implement writer-specific policy for any standard restrictions on intermediate section lengths and alignment, and validate against external readers with >4 GiB sparse fixtures. Level 1/2 writer rejects a file needing multiple sections. Verify actual file data bytes, not only lengths.

A bounded multi-extent index may be built lazily or stored in scratch as stable `{logical_start, logical_len, physical_start, physical_len}` rows. Binary search is useful for random `read_at` on enormous multi-section files; scanning from the beginning for each read is unacceptable. For ordinary single-section files, no index is needed.

### 9.3 Interleaved recording

For an interleaved section with `U = file_unit_size` and `G = interleave_gap` measured in logical blocks, logical data block `k` is generally stored at:

```
physical_block = extent_lba + xar_blocks
               + floor(k / U) * (U + G)
               + (k % U)
```

This is meaningful only when `U > 0` and the format's rules permit the combination of `U`, `G` and section size. `U=0,G=0` is non-interleaved. The gap sectors are not file bytes; don't expose them through `fsimg_reader_read_at`. Offset inside each block is `file_offset % block_bytes`. `resolve` should return at most the remaining bytes of that file-unit run, never a single contiguous extent across an interleave gap. Validate the **last** physical block by formula, not with `data_len` contiguous arithmetic; require no overlap with reserved/metadata ranges.

Writer v1 **does not generate interleaving**. Reader support may be deferred after structurally recognizing it and returning `FSIMG_EUNSUPPORTED`, without accidentally treating interleaved content as contiguous.

### 9.4 Sparse and compressed representations

**RRIP `SF`** extends the file data model with sparse-file metadata; an ISO sector filled with zeros is not automatically a sparse hole. **zisofs** uses `ZF` and a separate compressed-file header/block index with compressed byte ranges. If `ZF` is present, a reader must either decompress/validate via an explicitly implemented decoder or say it cannot transparently expose the file. Returning compressed bytes under the logical uncompressed size is a correctness bug. A future decoder can map holes to `FSIMG_RANGE_ZERO` and compressed blocks via a bounded decompression path. Do not add zlib as a default dependency without revisiting `fsimg`'s dependency policy.

## 10. Rock Ridge and SUSP

**Sources:** [IEEE P1282/RRIP draft version 1.12](https://people.freebsd.org/~emaste/rrip112.pdf), System Use Sharing Protocol (SUSP, companion IEEE P1281 draft), independent BSD readers [FreeBSD cd9660](https://github.com/freebsd/freebsd-src/tree/main/sys/fs/cd9660) and [libarchive ISO reader](https://github.com/libarchive/libarchive/blob/master/libarchive/archive_read_support_format_iso9660.c). IEEE draft wording is copyrighted: this section independently restates technical layout facts, not its prose.

### 10.1 SUSP is a framing protocol, not a synonym for Rock Ridge

ISO directory-record system-use bytes are ordinarily opaque. SUSP describes tagged entries there and continuation in another logical-block region. RRIP uses SUSP to encode POSIX metadata. Therefore **do not identify Rock Ridge merely because printable bytes `NM` or `PX` appear in a random directory record**.

Every SUSP entry begins with four bytes:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | ASCII signature, e.g. `SP`, `CE`, `ER`, `ST`, `PX`, `NM` |
| 2 | 1 | Total length including header; >=4 unless a type requires greater length |
| 3 | 1 | Entry version (typically 1) |
| 4… | variable | Type-specific contents |

Known SUSP entries:

| Entry | Common total bytes | Payload / purpose |
|---|---:|---|
| `SP` | 7 | Check bytes `0xBE 0xEF` followed by a one-byte **skip** value. Establishes SUSP use. Usually in root `.` system-use. |
| `ST` | 4 | Explicit end of SUSP entry stream. Bytes after `ST` are not further entries. |
| `PD` | variable | Padding marker. Its content is not metadata. |
| `CE` | 28 | Continuation LBA (`733`), byte offset in block (`733`), continuation byte length (`733`); references more SUSP entries. |
| `ER` | variable | Declares extension identifier/description/source and extension version; recognizes RRIP by a known supported identifier, not only by an `RR` bit. |
| `ES` | 5 | Extension selection where several SUSP extensions coexist; interpret only when valid. |

`SP`'s skip value is a number of system-use bytes reserved for other purposes before SUSP entries in subsequent directory records. It is neither a skip over logical sectors nor a skip over the `SP` entry itself. The initiating root-dot entry needs specific handling, and the skip is applied consistently to the appropriate remaining record system-use areas. In the writer default, use skip=0.

`CE` fields are each standard dual-endian `733` values. The LBA is a logical block address; the offset is **byte offset within the referenced block**, and byte length may span blocks. Validate `offset < block_bytes`, checked `lba*block_bytes+offset+length`, and exact source range before traversing it. `CE` continuation is **not** the same concept as `NM` name continuation or ISO multi-extent data.

### 10.2 Required safe SUSP decoder

1. Discover SUSP at the root directory's **self** record using `SP` and, for RRIP, an appropriate `ER`. Validate check bytes, versions and skip.
2. When SUSP is enabled, use a bounded iterator over the system-use area of each applicable directory record, exposing typed entries without allocating per-TLV objects.
3. Validate `entry_length>=4`, required type-specific lengths, version and no overrun before any payload interpretation; consume exactly entry length.
4. On `CE`, validate region bounds, enforce an explicit **total decoded bytes** quota, continuation-depth quota and total entry quota, and detect cycles by exact `(LBA, offset, length)` identity in a small charged work set or scratch-backed set. Also prevent overlapping recursion/aliasing that would lead to repeated processing.
5. Stop on `ST`; skip `PD` correctly; safely ignore unknown **non-critical** extension signatures while validating framing. Unknown semantics that change file content or namespace must cause unsupported, not silently wrong extraction.
6. Apply the selected extension's namespace and POSIX metadata **only** after every required record is structurally decoded and the extension handshake matches a supported version.

Keep `struct susp_cursor` as small as possible; a continuation should be streamed through the common positional I/O layer, not slurped into a huge allocation. One record can have multiple CE fragments. Work quotas count decoded bytes, not only final metadata length. Malformed `SP` or missing `ER` must not cause a parser to reinterpret arbitrary metadata as POSIX names.

### 10.3 Rock Ridge entry layout map

For RRIP 1.12, SUSP header size is 4. Numeric fields follow the ISO dual-byte-order rules; entries below are described with zero-based offsets relative to SUSP entry start.

| Tag | Size | Contents / intended handling |
|---|---:|---|
| `PX` | **44** | `mode(733)` at 4–11, `nlink(733)` at 12–19, `uid(733)` at 20–27, `gid(733)` at 28–35, `inode/file serial(733)` at 36–43. Old disc variants may use a shorter `PX` without serial: support only with a clearly named compatibility profile. |
| `PN` | 20 | Device number high/low words, two `733` quantities; for device nodes. Current `fsimg` kinds lack devices: recognize but reject extraction into unsupported kind. |
| `SL` | variable ≥5 | Byte 4 entry flags, then a sequence of component records `(flags:u8, length:u8, contents)` for symbolic link target. |
| `NM` | variable ≥5 | Byte 4 flags followed by name bytes. Multiple `NM` records concatenate; never NUL-terminate source bytes. |
| `CL` | 12 | `733` child link LBA for relocated directory. |
| `PL` | 12 | `733` parent link LBA for relocated directory. |
| `RE` | 4 | Marks a directory record relocated in the physical ISO hierarchy. |
| `TF` | variable ≥5 | Byte 4 timestamp flags, followed in prescribed order by 7-byte short or 17-byte long times. |
| `SF` | variable | Sparse-file encoding with virtual length/table metadata; defer until a separate verified sparse decoder. |
| `RR` | usually 5 | Historical RRIP field-presence summary; optional compatibility clue, not sole authority that an extension exists. |

`PX` is mandatory according to RRIP for records in an RRIP hierarchy; `NM`, `TF`, `SL`, `CL`, `PL` are conditional. Check duplicated or contradictory single-instance metadata and reject ambiguous types rather than take arbitrary precedence. The **file serial number** plus POSIX mode can identify hardlink candidates, but common extent identity and link counts must be independently consistent before treating two entries as one object.

### 10.4 `NM`: alternate POSIX names

The `NM` flags byte uses bit 0 as *continuation*, bit 1 as *current directory*, bit 2 as *parent directory*; other bits are reserved or explicitly special depending on RRIP variant. A name is formed by **concatenating bytes from successive `NM` records until continuation clears**. Restrict the total produced component to `fsimg`'s public name budget and strict UTF-8 policy. If a disc records POSIX names as arbitrary non-UTF-8 octets, the core UTF-8 API cannot losslessly present them; do not silently reinterpret bytes with the host locale. Options: `FSIMG_EUNSUPPORTED` for forced RRIP, or a future explicit reversible escaping policy; a nonforced reader may select a coherent valid Joliet/PVD hierarchy when RRIP cannot be represented.

`NM` name bytes should never contain `/` or NUL as literal component content; reject `.` and `..` except their special reserved-record semantics. Check the entire displayed RR namespace, not just the ISO aliases; distinct ISO identifiers can legitimately have colliding RR names on a malformed image. The common prepared-directory sorter already detects such collisions when provided the correct RR byte-exact `keys`.

### 10.5 `SL`: symbolic links

`SL` has an entry-flags byte; its component records have `(flags, length, bytes)` framing. Component flags include continue, current `.`, parent `..`, root `/`, with additional implementation-specific markers. A continued component concatenates its fragments **without inserting a slash**. Completed components join with `/`; a root component establishes an absolute path. Continuation across `SL` records is distinct from component continuation inside one `SL`. The reader builds a bounded **target string**, then presents a `FSIMG_ENTRY_SYMLINK` entry whose size is target-byte length and whose content is the target, as `fsimg` expects. Reject oversized targets (> existing 1023-byte public maximum), malformed flags, embedded NUL, or paths that cannot be represented without loss. **Never follow a symbolic link while extracting.** The extraction layer already handles descriptor-relative safe operations; the ISO driver must only report the target.

Special *file kinds* such as FIFO can be derived from `PX` mode, and must respect capability masks. A Rock Ridge symlink is *not* a regular ISO file whose data length describes target text.

### 10.6 `TF`: POSIX timestamps

The TF flags in bit positions 0–6 indicate which timestamps are carried: creation, modification, access, attributes/status change, backup, expiration and effective. Bit 7 selects long 17-byte timestamps instead of short 7-byte timestamps. The time fields appear in flag order. Verify exact expected byte count from the flags and selected width. In `fsimg`: map modification→`FSIMG_META_MODIFIED`, access→`FSIMG_META_ACCESSED`, creation→`FSIMG_META_CREATED`, attributes change→`FSIMG_META_CHANGED`; only set `valid` for actually recorded values. Preserve per-field time basis, offset and precision; do not fall back from missing `TF` values to synthetic zero or invented current time.

**Writer:** prefer long TF form if centisecond precision and time-zone offset are representable and requested; use short form when sufficient; date rounding reports an optional loss where relevant. The existing metadata-loss rules already distinguish required vs optional fidelity.

### 10.7 `CL`, `PL`, `RE`: deep tree relocation

Classic ISO requires a shallow physical hierarchy. RRIP reconstructs deeper logical trees by physically relocating directory extents (often into an internal `RR_MOVED` directory) and leaving a link at the intended parent:

- `CL` on a placeholder record points to the true relocated directory.
- `PL` on the relocated directory's parent record gives its original logical parent.
- `RE` on relocated physical directory marks the relocation so it should not appear twice in the logical RR namespace.

**Reader invariant:** follow a `CL` only when it resolves to a valid moved directory, with consistent `RE/PL` relationship and in-range address. The RR logical walker yields the child under its original parent, suppresses its separate physical moved copy, and detects loops. The ISO PVD walker, when explicitly selected, can expose the conforming physical tree as such. Do not take `RR_MOVED` as a magic safe filename alone; extension tags and pointer consistency determine semantics.

**Writer option:** first support ordinary RR metadata with physical depth within ISO limits. For deeper source trees either implement relocation as a planned graph of physical directories (the full CL/PL/RE protocol) or report `FSIMG_ERANGE`/`FSIMG_EUNSUPPORTED`. Do not silently violate ISO depth while advertising Level 1/2. RRIP relocation is the most complex Rock Ridge feature; keep it in a separately testable module.

### 10.8 Other metadata, IDs and compatibility

`PN` device-number reconstruction is useful for auditing media but current `fsimg` deliberately supports only FILE/DIRECTORY/SYMLINK/FIFO; exposing a device as an ordinary file would be misleading and dangerous. `SF` support must include declared virtual size, sparse index traversal and checksum/bounds validation where applicable. A number of discs use historical RRIP `PX` 36-byte records, nonstandard ER identifiers, `AAIP` `AL`, `ZF` compressed-file markers or Apple extensions: detect, document support subset and test each explicitly rather than pretending all RRIP media are one uniform encoding. For read-only fallback, unsupported POSIX metadata can sometimes be ignored in pure `iso` mode, but **never ignored if it changes file content** under a promised transparent read mode.

### 10.9 RRIP writer layout and continuations

Create a SUSP plan for every directory-record occurrence *before* assigning its final length. Prioritize `SP`/`ER` for root self record, `PX` for each RRIP-described entry, `NM` for non-primary names, `TF` for supported timestamps, `SL` for symlinks. Decide which TLVs fit in the record's available system-use bytes (subject to 254-byte full-record maximum) and place remaining bytes in separately allocated, logically contiguous **CE continuation areas**. No TLV may be split through an arbitrary byte boundary; `NM`/`SL` have their own structured fragmentation semantics. The presence of `CE` itself consumes **28 bytes** in the original record, so a simplistic 'fill to 254, then add CE' layout overflows. A two-pass or bounded fixed-point sizing step solves this while preserving reproducibility:

1. Compute canonical metadata TLVs and directory-identifier lengths.
2. Reserve CE marker capacity in entries that need overflow.
3. Emit non-overflow TLVs in record and spill remainder to stable continuation streams/blocks.
4. Assign CE block locations and `733` addresses.
5. Emit directory entries and CE streams, including `ST` when required by selected convention.
6. Re-parse the resulting SUSP streams with an independent test model.

A writer-generated ordinary file can use one physical content payload referenced by primary and Joliet directory records. Rock Ridge uses the primary record payload, not a third copy of file content.

## 11. Joliet

**Normative reference:** [ECMA-119 (2025), Annex C](https://ecma-international.org/wp-content/uploads/ECMA-119_6th_edition_december_2025.pdf), adapting the [Microsoft Joliet specification](https://pismotec.com/cfs/jolspec.html).

### 11.1 Recognizing a Joliet descriptor

Joliet appears as an SVD (`type=2, descriptor_version=1`) with a recognized three-byte escape sequence beginning at SVD offset 88:

| Escape bytes | ASCII rendering | Joliet graphic set designation |
|---|---|---|
| `25 2F 40` | `%/@` | UCS-2 Level 1 |
| `25 2F 43` | `%/C` | UCS-2 Level 2 |
| `25 2F 45` | `%/E` | UCS-2 Level 3 |

Byte 7 (SVD volume flags) needs validation; default bit 0 is zero. Each Joliet descriptor identifies its own root record, L/M path tables, volume label and directory tree. A valid type-2 descriptor without one of the three registered escapes is **not automatically Joliet**.

### 11.2 Character representation

Encode every ordinary Joliet string as **big-endian UCS-2** (one 16-bit code unit per BMP scalar), not host-endian UTF-16 or UTF-8. Reject surrogate code points and non-BMP scalars on writing if strict UCS-2 requested; Unicode replacement causes silent filename loss and collisions. Filenames/directories can use up to **64 UCS-2 characters / 128 identifier bytes** for the Joliet extended profile. Control U+0000–U+001F and reserved `* / : ; ? \\` are not allowed in normal Joliet filenames under Annex C; handle those via deterministic aliases if a RR path can preserve actual names or reject Joliet writing.

Special self/parent directory names remain single-byte `0x00` / `0x01`; treating these as two-byte `00 00` / `00 01` breaks interoperability. For regular files, structural period and version separators are 16-bit UCS-2 `00 2E` and `00 3B` when required; do not parse semicolons by scanning odd byte positions. Joliet allows directory names containing extensions and deeper directory hierarchies than the base ISO eight-level limit. The Joliet annex provides a path-length budget of **240 bytes** by its accounting; enforce it for writer emission along with `fsimg`'s own depth/path quotas. These limits are separate from the common ISO directory-record byte limit (254).

### 11.3 Sorting, names and payload sharing

Sort Joliet directory records and Joliet path tables by their **encoded 16-bit code points** with Annex C's zero-padding comparison rules. A filename's UTF-8 byte count is not its UCS-2 identifier byte count. Verify equivalence and collision freedom in the Joliet tree independently from the primary/RR tree. The same physical data blocks may be referenced by separate directory records in primary and Joliet trees without duplicating payload bytes. The SVD's **path tables and directory records do require their own metadata storage**; merging them physically with primary path tables is not valid unless their bytes truly coincide under all format constraints.

**Mixed RR + Joliet:** The writer emits an RRIP-enriched PVD hierarchy and a separate clean Joliet hierarchy; reader chooses one at open. Do not synthesize POSIX ownership for Joliet by searching the RR tree for same-name paths: the projections may rename paths differently. Instead, any POSIX metadata in an SVD tree needs an explicitly supported extension present in that tree.

### 11.4 Compatibility policy

Some writers emit malformed surrogate-containing UCS-2 or name fields longer than the nominal Joliet bounds. The default `fsimg` mode should not silently reinterpret invalid UCS-2 as UTF-16 and claim strict Joliet support. A future opt-in compatibility mode may support UTF-16BE surrogate-pair names as a documented extension, but would not call such discs strictly conforming. Test forbidden codepoints, odd byte lengths, BOM misuse, bad escape, conflicting multiple Joliet SVDs, duplicate UCS-2 names, and host-name collisions after UTF-8 conversion.

## 12. El Torito boot catalogs and images

**Sources:** [El Torito specification v1.0](https://misc.manty.net/eltorito.pdf); permissive [edk2 `ElTorito.h`](https://github.com/tianocore/edk2/blob/master/MdePkg/Include/IndustryStandard/ElTorito.h) (`BSD-2-Clause-Patent`); [UEFI specification on optical boot](https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html).

### 12.1 Boot Record Volume Descriptor

This is a type-0 ISO descriptor, with standard `CD001` and version 1. A boot record's type-specific layout:

| Offset | Length | Value |
|---:|---:|---|
| 0 | 1 | Type 0 |
| 1–5 | 5 | `CD001` |
| 6 | 1 | Version 1 |
| 7–38 | 32 | Boot system identifier; for El Torito ASCII `EL TORITO SPECIFICATION` padded |
| 39–70 | 32 | Boot identifier/application name or unused, dependent on scheme |
| 71–74 | 4 | **Little-endian** LBA of boot catalog (for El Torito) |
| 75… | remainder | Boot-system-defined/reserved |

Important: the catalog is **not necessarily a file in the ISO directory hierarchy**. It is a data extent referenced by this boot descriptor. One image may have multiple boot records or multiple platform entries; don't assume one BIOS boot entry or that the first catalogue entry is suitable for UEFI.

### 12.2 Validation entry (catalog bytes 0–31)

A catalog consists of 32-byte units, begins at the LBA named in the boot descriptor, and may extend beyond one 2048-byte block.

| Catalog offset | Size | Content |
|---:|---:|---|
| 0 | 1 | Header ID `0x01` |
| 1 | 1 | Platform ID: e.g. `0x00` x86 BIOS, `0x01` PowerPC, `0x02` Mac, `0xEF` EFI |
| 2–3 | 2 | Reserved zero |
| 4–27 | 24 | Vendor/developer identification string |
| 28–29 | 2 | Little-endian validation checksum word |
| 30–31 | 2 | Magic bytes **`55 AA`** |

Checksum: the sum of all 16 little-endian words in the 32-byte validation entry, **including checksum itself**, must be zero modulo 65536. Verify exact signature, checksum and in-volume bounds before reading other catalog entries.

### 12.3 Initial/default and section boot entry (32 bytes)

| Offset | Size | Content |
|---:|---:|---|
| 0 | 1 | Boot indicator `0x88` bootable or `0x00` not bootable |
| 1 | 1 | Media type low nibble: 0 no-emulation, 1 1.2 MB floppy, 2 1.44 MB floppy, 3 2.88 MB floppy, 4 hard disk; upper bits reserved/extension-defined |
| 2–3 | 2 | Little-endian load segment (BIOS-specific; zero has conventional meaning) |
| 4 | 1 | System type byte |
| 5 | 1 | Reserved zero |
| 6–7 | 2 | **Little-endian 16-bit virtual load sector count** |
| 8–11 | 4 | **Little-endian image starting LBA** in CD/ISO 2048-byte sector units |
| 12–31 | 20 | Reserved or conditional platform extension fields |

For BIOS no-emulation loading, virtual load sector counts are conventionally 512-byte units. **They are not ISO LBA units.** EFI firmware may treat an EFI no-emulation image as a FAT EFI System Partition, with UEFI-specified count semantics; never calculate the complete image allocation size *only* by assuming the BIOS load count is its total size. A boot image can be much larger than its initial bootstrap load. Preserve a separate `image_extent_length` from writer input/layout when emitting, and use catalog data for firmware load semantics.

### 12.4 Additional architecture sections

After validation and initial entry, zero or more **section header** records use indicator `0x90` (more follow) or `0x91` (last), a platform ID, little-endian 16-bit number of following section entries, and 28-byte identifier. Boot entries then use the same basic layout as the initial entry, with possible extension entries. Validate section-count arithmetic and structure boundaries; do not scan unbounded catalog padding as if it were valid entries. Platform `0xEF` denotes EFI, and real-world hybrid media may contain both x86 BIOS (`0x00`) and EFI entries (`0xEF`).

### 12.5 Proposed read API and boot-image writer

**Reading:** ISO `verify()` checks catalog structure and references, and exposes boot metadata as **volume information or structured inspection**, *not* as fake filesystem entries. Since the current `fsimg_volume_info_t` has no boot-entry arrays, a versioned optional `fsimg_reader_get_boot_info()` would be a separate design change. Do not leak hidden boot entries through `fsimg ls`. Exporting a boot image's bytes by raw LBA can be a future explicit opt-in tool command.

**Writing:** take one or more explicit boot-image byte streams with platform ID, emulation type, system type, load segment, virtual load sector count and flags; reserve aligned boot-image extents, write their content, construct one checksummed catalog, and emit type-0 descriptor before the PVD and SVD. If the boot image is also listed as an ordinary file, share the **same** allocated data extent; don't duplicate its bytes or assume the file path is required by El Torito. A BIOS boot image being present does not mean a created disc boots without a correct boot program: `fsimg` should advertise El Torito **format integrity**, not the execution correctness of arbitrary bootloader bytes.

Do not generate an `isohybrid` MBR/GPT, protective MBR, EFI System Partition GPT view, or GRUB/ISOLINUX boot-info-table patch automatically; these are distinct features and may conflict with zero-filling the system area. The default writer leaves system area deterministic and zeroed; bootable *CD* image and bootable *USB-disk hybrid* image are different acceptance targets.

## 13. Additional extensions and optical-image variants

ISO 9660 has a large compatibility surface. This section separates **extensions that alter a selected file's actual bytes** from metadata that can safely remain opaque until implemented.

| Variant | Detection / important semantics | Scope recommendation |
|---|---|---|
| ISO 9660 enhanced / 1999-style EVD | Type 2 **descriptor version 2**, with relaxed name rules and agreed character encoding; a distinct namespace. | Read after PVD/Joliet; write optional after compatible oracles. |
| Rock Ridge 1.10/1.12 legacy dialects | Differences in `PX` length, extension identifiers, SUSP records, historical `RR`; use explicit compatibility tests. | Read common variants, write current conventional form. |
| zisofs | Rock Ridge-associated `ZF` marker + compressed stream with a zisofs header, block size and offset table; logical and stored sizes differ. | Strictly unsupported until decoded; never read compressed bytes as raw payload. |
| AAIP | ACL/xattr extension (`AL` and other tagged system-use records) used in optical authoring. | Metadata optional; verify framing; omit writing until public API has ACL/xattrs. |
| Apple ISO/HFS hybrid | Apple-specific SUSP/apple extensions, additional partition maps/HFS catalog in system area. | Read valid ISO hierarchy; don't overwrite hybrid partition/system-area metadata on a rewrite attempt. |
| CD-ROM XA (`CD-XA001`) | XA system-use data, Mode 2 forms and distinct sector payload; some CD-XA labels occur around descriptor offset 1024. | Detect, inspect, treat unsupported raw-sector formats separately. |
| XA interleaving | Interleaved `file_unit_size/gap_size`, possibly multimedia sector forms. | Reader may support ordinary ISO interleaving; not general CD-XA raw data. |
| Video CD / SVCD | ISO hierarchy plus XA Mode2 Form2 streams and MPEG layout conventions. | Basic names possible; correct raw stream extraction requires CD transport semantics. |
| DVD/Blu-ray UDF / ISO bridge | UDF structures per ECMA-167/OSTA independent of ISO file tree. | Separate UDF driver; valid ISO portion can still be read. |
| Multisession | Session-specific descriptor sequences; globally increasing LBA addressing, replacement tree in later session. | Read explicit session only after an input representation supplies session map; no auto-guess from padding. |
| ISO multi-volume set | Volume sequence number refers to one of multiple volumes; file data can be elsewhere. | Single-volume reader rejects external referenced sections clearly. |
| `isohybrid` | BIOS/UEFI MBR/GPT hybrid system area and reused ISO payload. | Read ISO; write only with explicit hybrid-layout project. |
| High Sierra / pre-ISO prototypes | Different on-disc IDs/layout despite superficial resemblance. | `FSIMG_EFORMAT` or `FSIMG_EUNSUPPORTED`, never reinterpret as ISO blindly. |
| Compressed `.cso`, `.zso` | Container-level chunk compression wrapping a virtual cooked ISO. | Separate seekable `fsimg_io_t` adapter if later required. |

**Extension selection principle:** if an extension affects *names or metadata*, a caller can choose a coherent alternative tree (plain ISO/Joliet) provided the chosen view is consistent. If it affects *file data representation* (compression, XA raw payload), the selected reader must decode it correctly or fail for that entry, rather than lie about content.

## 14. `fsimg` reader driver design

### 14.1 File structure and integration

Proposed code organization (not existing code):

```text
lib/formats/iso9660/
    iso9660.h              private decoded types, limits and namespace enum
    descriptor.c            fsimg_format_t, variants, probe
    volume.c                descriptor inventory, root selection, open/close
    records.c               numeric/time/directory-record decoding
    path-table.c            endian-dual path-table decode + crosschecks
    susp.c                  tagged entry framing + CE cursor
    rockridge.c             NM/PX/TF/SL/CL/PL/RE semantics
    joliet.c                UCS-2BE conversion and keys
    names.c                 ISO versions, alias handling and collision keys
    directory.c             prepare/replay/keys/key
    content.c               single/multiple/interleaved resolve
    verify.c                metadata ownership, structure, optional payload
    write-plan.c            hierarchy + alias/record/CE layout
    write-emit.c            deterministic sectors/descriptors/catalog/data
    eltorito.c              boot record and catalog verification/planning
```

A small number of files can be combined initially; **the important separation is between byte decoding, selected hierarchy semantics, SUSP framing, traversal, and writer planning**. Register one `fsimg_iso9660_format` descriptor in [`lib/formats/format.c`](https://github.com/stass/fsimg/blob/master/lib/formats/format.c), following [`lib/formats/fat/descriptor.c`](https://github.com/stass/fsimg/blob/master/lib/formats/fat/descriptor.c). Keep private `iso9660_*` symbols and BSD-2-Clause headers consistent with `specs/style.md`.

### 14.2 Probe / open / namespace selection

A practical state machine:

```text
probe(prefix):
    if shorter than initial recognizer: no claim
    inspect bounded descriptor slots beginning at sector 16
    if no plausible descriptor identity: FSIMG_EFORMAT
    if type0 boot descriptor precedes PVD: continue
    if valid primary signature within bounds: claim
    if recognized ISO descriptor chain but required descriptor beyond prefix:
        FSIMG_EUNSUPPORTED (or defer complete validation to open)
    if contradictory claimed geometry in prefix: FSIMG_ECORRUPT

open(reader):
    scan bounded descriptor set from I/O source
    select supported root according to forced/auto policy
    set volume info: readonly geometry/label/profile/time basis
    bound volume space and content domain
    parse selected root record; discover RRIP if relevant
    initialize directory identity and optional extent index
    do not enumerate all directories yet
```

The common open/probe division means the probe must be a pure function of prefix bytes and volume size. To detect a PVD after boot records, reserve a reasonable bounded prefix (e.g. 128 KiB) and capture test fixtures where the PVD begins at descriptor slot 17 or 20. If there is no recognizable descriptor **at all** in the prefix, report `FSIMG_EFORMAT`, not speculative `FSIMG_EUNSUPPORTED` for an invisible descriptor. Large prefix affects **all readers' auto-probe memory cost** because `fsimg` currently sizes to maximum registered `probe_bytes`: benchmark this and consider staged probing as an independent common-core proposal if 128 KiB is undesirable.

### 14.3 Reader operations exact mapping

| Existing callback | ISO-specific responsibility |
|---|---|
| `open` | Discover descriptor inventory; select namespace; bound volume; load root state; optional Rock Ridge handshake. |
| `close` | Release driver state and extent/path-table indexes, including partial-open failure. |
| `prepare` | Read the current directory's described records, validate framing and consistency, group multi-extent files, reconstruct RRIP names/relocations when selected, call `fsimg_directory_admit` for each visible logical child. Do not expose a partially validated directory. |
| `replay` | Reparse charged scratch record payload and validate original extent/metadata; set `reader->entry` excluding path and private current locator, and set `reader->initialized` only for ordinary correctly represented files. |
| `keys` | Derive canonical namespace keys for the selected hierarchy (ISO names+versions, byte-sensitive RR or Joliet), including aliases only when they won't collapse distinct entries. |
| `key` | Derive query key under exactly the same rule; invalid name encoding is `FSIMG_EINVAL`, not a silent not-found. |
| `ambiguous` | Diagnostic: `ambiguous ISO 9660 namespace` with selected namespace and conflicting names. |
| `resolve` | Find checked logical-to-physical file content extent; group multiple sections; split at gaps and file boundaries; no allocation on common sequential path. |
| `verify` | Header/structural/payload stages, check selected and optionally all directory hierarchies, compare L/M path tables and physical metadata ownership, stream file content when requested. |

The common prepared-directory stack guarantees no entry appears until siblings have been validated. It already externally sorts opaque keys and replays stable records. Build on this rather than implement a parallel recursive walker.

### 14.4 Private identity and storage domain

Maintain separate identities for:

- **Directory-record occurrence:** selected hierarchy, directory extent, record-relative byte offset, file-section-group index. Used for replay/namespace.
- **Logical object:** RRIP inode serial and mode when trustworthy; otherwise unique occurrence or intentional common payload identity.
- **Physical occupied span:** `[start,end)`, length, purpose (PVD/path table/directory/CE/catalog/boot/file), allowed aliases.
- **Logical parent:** RRIP relocated tree pointer, not necessarily the physical `..` record.

`reader->content_domain` should bound all ordinary ISO file data in the declared volume; for a physical region containing special subareas, further per-extent checks enforce that data does not trespass onto path tables/descriptors except documented sharing. The common `resolve` callback is a good match for ordinary extents and multiple extents. Metadata scratch indexes prevent path-table and file-extent enumeration from requiring memory proportional to the entire image.

### 14.5 Public API and reporting

The current public [`fsimg_format_info_t`](https://github.com/stass/fsimg/blob/master/include/fsimg.h) lists entry kind masks, metadata masks, size masks and maximum representable names. Suggested `iso9660` capability:

- `sector_size_mask`: at least `(1u << 11)` (2048) for the cooked ISO writer; reader support for exotic logical block sizes requires careful and separately documented limits.
- `write_allocation_unit_size_mask`: `1u << 11`; the image is read-only, so do not invent a FAT-style cluster allocation unit.
- Basic PVD mode: FILE|DIRECTORY, metadata at minimum volume record-date interpretation + hidden; ordinary ISO has no POSIX symlinks/FIFOs.
- RRIP *writer profile*: FILE|DIRECTORY|SYMLINK|FIFO and genuinely written MODE/OWNER/MODIFIED/ACCESSED/CREATED/CHANGED as supported; device nodes absent unless the public kind enum is extended.
- `max_file_bytes`: distinguish single-section `UINT32_MAX` from writer-supported multi-section checked sum; report actual supported maximum for each profile rather than speculative near-8-TiB.
- `max_name_utf16_units`: per namespace; RRIP UTF-8 names are not inherently UCS-2, so capability documentation must explain source name/byte bound rather than conflating units.
- `max_label_bytes`: PVD label field = **32 bytes**; Joliet SVD label is 32 **UCS-2BE bytes** (up to 16 BMP codepoints), if using this field.
- `FSIMG_VOLUME_FREE`: **not** advertised, because ISO is not an allocation-writable filesystem. `free_bytes=0` should never imply a free-space query succeeded. `FSIMG_VOLUME_UUID`: not advertised; PVD volume label and creation timestamp are **not UUIDs**. `clean` flag is not on-disc clean status; do not assert a journal-integrity feature ISO lacks.

Profile naming can be compact (`iso9660` as one listed format, `volume.profile="iso9660/rrip-1.12"`, etc.) while allowing read namespace preference through new options. Avoid changing public ABI without a versioned struct plan. For `fsimg_write_options_t`, reject irrelevant format-specific knobs (`volume_serial`, UUID, FAT root count, byte order, reserve percent when non-default); `min_free_bytes` is nonsensical for a non-writable ISO and should not be accepted as free capacity. `sector_bytes=0` should choose 2048 for ISO rather than the generic FAT default. Preserve `volume_bytes` as a fixed output image-size request, with predictable trailing zero fill.

### 14.6 Error classification and resource budgets

Define per-driver explicit limits, all charged against the existing `fsimg_ctx` and session work budgets:

| Resource | Bound proposal | Rationale |
|---|---|---|
| Descriptor probe | 128 KiB | Covers ordinary descriptor sequences with many boot/SVD records; revisit common probe staging. |
| Descriptor records parsed | 64 | Bounded and explicit; larger valid sets recognized as unsupported, not scanned forever. |
| One SUSP entry | 255 bytes | Encoded length field bound. |
| One logical RRIP name | Public component byte limit | Avoid unbounded `NM` concatenation. |
| One symlink target | Existing 1023-byte `fsimg` limit | Matches current API. |
| Total CE expanded bytes per entry | e.g. 64 KiB default + charged quota | Stops continuation amplification. |
| CE nesting | e.g. 16 | Cycle/exponential expansion defense; tune from fixtures. |
| Path-table bytes | Volume-bounded + session work quota | Never malloc whole volume's table. |
| Directory depth | Existing `fsimg_ctx` depth limit | Independent of RRIP physical-relocation count. |
| Extent-index rows | Scratch-backed, work-budgeted | Predictable random reads for multi-extent. |

These numerical limits are **proposed implementation budgets, not ISO standard maxima**. Put values in one driver-limits header, expose clear `FSIMG_ELIMIT` when an otherwise valid volume exceeds a configured budget, and add tests that trigger each budget exactly and just beyond the boundary.

## 15. `fsimg` writer and deterministic layout design

### 15.1 Writer contract

The existing writer uses `validate`, `plan`, `emit`, `free`; common code takes a source catalog, seals on `plan`, supports `fsimg_plan_info_t`, and initializes/defines every emitted sink byte. ISO is naturally **plan-first**, because directory records contain their children's extent addresses, PVD contains path-table addresses, and CE references need final allocation addresses. A streaming tar-like single pass isn't the right model; `fsimg`'s existing architecture is.

Proposed high-level pipeline:

```text
source catalog (common; independently ordered from host traversal)
    -> validate requested profile & required metadata
    -> project every selected namespace (PVD + optional Joliet; RR tags on PVD)
    -> resolve deterministic names/aliases & duplicates
    -> size directory records and SUSP CE payloads
    -> compute ISO hierarchy depth & optional RR relocation graph
    -> generate path table records (L and M) and directory numbering
    -> assign every metadata extent and file section extent
    -> resolve all absolute LBA references + El Torito boot data/catalog
    -> validate the plan using the *same format predicates* as a reader,
       but preferably with separate typed preflight checks
    -> seal immutable plan; expose plan_info counters
    -> write initialized content + zero padding in deterministic extent order
    -> finalize sink through existing common durability/publication path
```

### 15.2 Allocation layout

A simple deterministic on-disc layout for writer v1:

```text
LBA 0..15          ISO system area (zero by default)
LBA 16..          optional El Torito boot descriptor
                   PVD
                   optional Joliet SVD(s)
                   descriptor set terminator
                   optional descriptor-set padding
                   primary type-L and type-M path tables
                   Joliet type-L and type-M path tables (if enabled)
                   primary directories in deterministic order
                   Joliet directories in deterministic order (if enabled)
                   SUSP continuation areas (CE)
                   optional El Torito catalog and boot images
                   payload data extents (shared between namespaces)
                   final sector/tail padding if requested
```

Actual directory vs payload placement is an implementation choice; all pointers are absolute logical block addresses. Align every extent to 2048 bytes, pad unused bytes to zero, and use exact path table *unrounded* lengths in descriptors. The source `epoch` and `FSIMG_WRITE_REPRODUCIBLE` control deterministic descriptor and recording times. Record no uninitialized structure padding, host inode numbers, random UUIDs, or current local time.

Alternative: reserve file extents before directory streams to simplify streaming source payload or allow source data copying in a single contiguous pass. Either way, `plan()` calculates a complete map and checks it before mutating a sink. No writer stage should need to seek back and patch already written bytes except as a deliberately documented optional optimization; existing emission order allows known descriptors to be emitted once.

### 15.3 Directory packing (the central algorithm)

For each namespace independently:

1. Determine every child's complete final encoded identifier including `;1` and `NM`/`SL`/`TF` records.
2. Build complete ISO records, maintaining the 254-byte limit and even length, with `CE` spill when needed.
3. Place the two mandatory self/parent records first; sort remaining children by ISO or UCS-2 identifier rules. Multi-section records for one file remain together in section order.
4. For each record: if its length exceeds `2048 - (current_dir_byte_offset % 2048)`, insert zero padding to the end of that sector, then emit the record at next sector start.
5. Round total directory data length to full logical sectors and store that length in its directory record and any parent/reference copies; backfill only in plan data, not a live sink.
6. Verify every directory (including root) has internally consistent self/parent pointers and the correct data-length field.

**Avoid quadratic fixed-point planning.** Directory record **width** is independent of most final LBAs (numbers have fixed byte width); only relocation/CE decisions alter graph topology. Therefore separate (a) deterministic name/TLV sizing from (b) fixed-width address resolution. A topological pass computes all directory byte lengths bottom-up when child extents are not yet known; a second linear pass assigns LBAs and fills all address bytes. CE spill decisions depend on fixed TLV sizes, not actual LBA numeric magnitude, enabling stable two-pass sizing without iteration over volume growth.

### 15.4 Hardlinks, duplicates and shared content

If RRIP hardlinks are supported, emit multiple PVD records with matching POSIX serial/link count while pointing to one shared content allocation. Detect source hardlinks by stable common source identity **only if catalog supplies it**; otherwise don't opportunistically merge distinct files solely because they have identical size/hash. Different source files with identical data can be deduplicated as an explicit optimization only after designing its collision/verification and accounting semantics; not needed initially. Primary + Joliet references to the **same logical source file** always reuse its one data allocation. A symlink is represented by RRIP `SL`, with no ordinary file payload.

### 15.5 Output sizing and plan accounting

`fsimg_plan_info_t` fields should have clear, non-overlapping meanings:

- `payload_bytes`: sum of logical source file contents once per logical file; hardlinks and parallel Joliet views don't multiply this.
- `metadata_bytes`: ISO descriptors, both path tables, directories, SUSP CE, boot catalogs and other structural bytes **as occupied extents** (record separately from padded tail/system-area if useful).
- `allocated_bytes`: actual sector-aligned occupied image span subject to `fsimg` common definition.
- `initialized_bytes`: ordinary file source byte extent, excluding padding. Compressed/sparse support later needs dedicated definitions.
- `volume_bytes`: exact emitted image byte size, including system area and any fixed requested padding.
- `free_bytes`: not a filesystem allocation/free-space concept for read-only ISO. A fixed output volume can contain trailing unused bytes but those must not be advertised as filesystem free blocks.
- `metadata_losses`: optional field losses counted under the existing `ALLOW_LOSS` contract, **never** silently truncating required names, file bytes, symlink targets or kinds.

A fixed `volume_bytes` below the computed minimum fails at plan. A fixed larger size yields deterministic zeroed slack; automatic sizing chooses the smallest sector-aligned size meeting the chosen writer layout policy. Out-of-range sector indices or counts fail *before* write.

## 16. Structural verification, corruption policy and security

### 16.1 Validation scopes consistent with `fsimg`

`fsimg` distinguishes `FSIMG_VALIDATION_HEADER`, `STRUCTURAL`, `PAYLOAD`:

| Scope | Required tests |
|---|---|
| Header | Descriptor identity and version, supported geometry, complete finite descriptor sequence, valid root descriptor framing and bounds, recognized requested hierarchy, boot descriptor pointer bounds. |
| Structural | Complete directory walk through common preparation; records and padding; dual-endian equality; root/self/parent semantics; path table equality and relationship; sorted namespaces; unique lookup keys; all extents within volume; file-section chains and interleaving; SUSP/CE references and cycle detection; Joliet syntax; RR relocations/hardlinks; El Torito validation/catalog. |
| Payload | All structural checks, then stream every declared file's logical byte content through `fsimg_reader_content`, verify successful reads/short-read behavior, optional independent test digests; compressed content only if supported. Boot image byte reads optional if boot inspection supports them. |

An ISO has no general-purpose per-file CRC or authenticated root hash. Therefore *payload verification can prove that bytes can be read and extents are structurally valid*, **not** that each original file's bytes match some authoritative digest. Test harness compares fixture digests externally; `verify(PAYLOAD)` should not misrepresent this as cryptographic integrity. Duplicate references to same physical content need efficient deduplicated reading when feasible, but verify every semantic reference's range and length.

### 16.2 Corruption vs unsupported vs limit

- `FSIMG_EFORMAT`: no ISO descriptor claim within bounded detection scope.
- `FSIMG_ECORRUPT`: impossible or inconsistent LE/BE, out-of-bounds extent, directory record crosses sector, directory cycle, SUSP malformed length or CE cycle, duplicated lookup key resolving differently, invalid path table relationship, malformed requested Joliet/El Torito structure.
- `FSIMG_EUNSUPPORTED`: valid unsupported EVD character encoding, CD-XA raw-sector layout, compressed file without zisofs decoder, multisession requiring unavailable session map, supported-format but unsupported record semantics.
- `FSIMG_ELIMIT`: otherwise valid image exceeding explicit configurable descriptor/CE/depth/work/metadata budget.
- `FSIMG_EIO`: underlying stable positional read/write failure, not reclassified as on-disc corruption.

The exact public enum and diagnostic formatting must follow [`specs/design-api.md`](https://github.com/stass/fsimg/blob/master/specs/design-api.md). Classification is stage-sensitive: a corrupt deep subdirectory is not necessarily discovered by `open` but is discovered by structural `verify` and by a list/extract that traverses it. A forced-namespace reader must never silently replace the selected namespace mid-walk when corruption is encountered.

### 16.3 Adversarial corpus and rejection rules

Protect against:

- Integer overflow in sector-byte multiplication, length rounding, `733` summation and pointer+length arithmetic.
- Missing PVD/terminator, malformed descriptor chaining and false positive `CD001` in arbitrary payload.
- Short/truncated image and reads beyond declared volume space.
- Malformed directory record lengths, `N` larger than record, missing align pad, nonzero sector padding, record crossing sector.
- Invalid identifier character codes, invalid version numbers, duplicate projected names, file+dir same displayed key, ambiguous versions and associated files.
- Directory cycles, pathological depth, duplicated logical-parent claims, invalid root/parent pointers.
- A multi-extent chain with no last record, changed name, duplicate section, length overflow, huge logical length but no backing blocks.
- Interleave geometry with `U=0,G>0` or absurd last block, data over metadata.
- XAR length that points past end, record/protection flags without XAR.
- SUSP TLV length 0/1/2/3, unsupported version, `SP` magic mismatch, `CE` chain cycle, self-recursion, overlapping continuation areas, fragmented NM/SL missing terminator, gigantic concatenated name.
- `PX` fake POSIX type mismatching ISO flags, malicious symlink targets, device special-file requests, RRIP relocated directory loops or physical copy exposed twice.
- Invalid Joliet escape, odd UCS-2 byte count, surrogate sequence, forbidden characters, duplicate UCS-2 names, aggressive name fallback collisions.
- El Torito bad checksum/signature, bogus section-entry counts, catalog pointing outside image, catalog pointing into itself, EFI entry with invalid image region.
- Host extraction `../` and absolute path components, symlink-to-outside, symlink traversal, special-file creation and TOCTOU source/path mutation. Do not implement extraction safety in the format driver: use existing safe common extraction helpers.
- Resource exhaustion through tens of thousands of very short records, large numbers of repeated CE sections, enormous directory trees/CE chains or intentionally long sort-key ties.

**Safety invariant:** no prefix of a corrupt directory is ever exposed as a successfully validated directory (the common preparation contract); all reported file byte ranges are inside the declared input domain; partial emitted files never become the trusted output path before publication.

### 16.4 Structural graph consistency

A useful verifier builds a **bounded external sorted interval stream** with `(physical_start, physical_end, owner_class, logical_object_id, hierarchy_id)`. It rejects overlaps between incompatible owners, but explicitly permits references to the same legitimate file data from primary/Joliet and RRIP hardlinks. This also avoids using `volume_space_size * 1 bit` RAM: scratch sort and interval sweep are preferred. For multiple extents, each section contributes one interval; for interleaving, each logical occupied unit (not gap) contributes its own intervals or a compact range expression. For CE, record exactly the continuation bytes; a CE region residing in a metadata block must be intentionally allocated and never accidentally be interpreted as payload.

## 17. Specification-driven rules, mutations and fuzzing

**Fit the existing Ruby harness, don't invent another.** [`specs/design-spec-testing.md`](https://github.com/stass/fsimg/blob/master/specs/design-spec-testing.md) already prescribes independently authored BinData byte layouts, a *total* decoder (explicit `Undefined` rather than raising), named logical rules, valid/invalid witness generation, corruption stage policies, witness coverage reports and independent tool adapters. Add an `iso9660` plug-in under the corresponding [`test/spec/`](https://github.com/stass/fsimg/tree/master/test/spec) conventions, maintaining the same cache / replay recipes / classification semantics as FAT and QNX6.

### 17.1 Suggested Ruby file structure

```text
test/spec/lib/fsimg_spec/iso9660/
    layout.rb         PVD, SVD, DIR, PT, XAR, SUSP, RRIP, boot catalog byte layouts
    model.rb          bounded total decoder; volume and hierarchy graph
    namespaces.rb     ISO/RR/Joliet logical lookup semantics
    rules.rb          registered named predicates/scopes/policies
    generators.rb     valid volume recipes and corpus parameter tables
    mutations.rb      one-field, one-boundary and paired-field corruptions
    witnesses.rb      valid and targeted-invalid witness families
    tools.rb          independent external-oracle adapters
    reports.rb        ISO-specific witness/extension coverage dimensions
fuzz/iso9660.c       libFuzzer ABI + configured full-structure and targeted seeds
```

These are **proposed paths**; fit names to the actual Ruby harness loader. The model must decode raw slices through Ruby's explicit LE/BE primitives or BinData, not import C structs from `fsimg`, and should contain independent acceptance rules rather than reproducing its implementation's choice of representation. It should *build* images with deliberately invalid both-endian halves, bad record padding, malformed CE links and broken path tables, not normalize them into valid ones accidentally.

### 17.2 Machine-readable rule inventory

Every rule should record `(id, reference, scope, valid_generator, targeted_mutation, required_detection_stage, independent_oracle_notes)` and have at least one valid and one invalid witness. Suggested names (examples, not copied predicates):

| Rule ID | Invariant / targeted negative witness | Earliest relevant stage |
|---|---|---|
| `ISO.VD.001` | Descriptor chain starts at session sector 16, with `CD001`/valid types and correct versions; flip signature | header |
| `ISO.VD.002` | PVD exists; insert type-0 boot before PVD, remove PVD | header |
| `ISO.VD.003` | Descriptor terminator found within declared supported bound; omit it | header |
| `ISO.VD.004` | 733/723 endian halves match; corrupt only BE half | header |
| `ISO.VD.005` | Volume space nonzero, block size supported, recorded extents bounded; adversarial overflow | header |
| `ISO.VD.006` | PVD root record 34 bytes, special root ID/directory flag; change record | header |
| `ISO.VD.007` | SVD/EVD version and flag semantics; change type-2 version | header |
| `ISO.VD.008` | Unknown supplemental descriptor does not invalidate a good PVD; add an unknown but framed SVD | header |
| `ISO.VD.009` | Volume set/sequence information consistent within supported single volume; request missing member | header |
| `ISO.VD.010` | Descriptor reserved padding valid in strict mode; mutate reserved field | header |
| `ISO.VD.011` | PVD logical size bounded by actual source slice; truncate file by one byte | header |
| `ISO.PT.001` | Path table L/M both present; remove M | structural |
| `ISO.PT.002` | Path table parent numbers valid, root index 1; parent 0 or child | structural |
| `ISO.PT.003` | L and M encode identical topology; alter one parent or extent | structural |
| `ISO.PT.004` | Path table records stay within declared byte length; truncate last name | structural |
| `ISO.PT.005` | Table sorted as prescribed; swap two siblings | structural |
| `ISO.PT.006` | Table agrees with actual directories; orphan or phantom entry | structural |
| `ISO.PT.007` | Optional duplicate path table agrees when present; corrupt duplicate | structural |
| `ISO.DIR.001` | Directory record length sane and even; length 0 misused inside live record | structural |
| `ISO.DIR.002` | Record not crossing 2048 sector boundary; move record to last 10 bytes | structural |
| `ISO.DIR.003` | Filename length and optional padding valid; change `LEN_FI` | structural |
| `ISO.DIR.004` | Sector padding zero; insert one nonzero byte | structural |
| `ISO.DIR.005` | `.` / `..` special IDs and links valid; flip parent's location | structural |
| `ISO.DIR.006` | Directory recursion acyclic; link child to ancestor | structural |
| `ISO.DIR.007` | Flags valid for directories/XAR; set directory+multi-extent | structural |
| `ISO.DIR.008` | File extent and data length within image; point at last block then overrun | structural |
| `ISO.DIR.009` | Entries sorted per encoded ISO identifier; unsort | structural |
| `ISO.DIR.010` | Distinct visible names unambiguous under chosen key rules; duplicate | structural |
| `ISO.DIR.011` | 7-byte timestamps/calendar/offset valid; month 13 / bad GMT | structural |
| `ISO.DIR.012` | Hierarchy depth/path bounds within selected profile; add 9th ISO level | structural |
| `ISO.NAM.001` | ISO Level 1 restricted 8.3 names; 9-character root file | structural |
| `ISO.NAM.002` | ISO Level 2 base + extension ≤30; overlong name | structural |
| `ISO.NAM.003` | File `;N` version in 1–32767; invalid digits / zero | structural |
| `ISO.NAM.004` | Two versions do not become same display name silently; `;1`, `;2` | structural |
| `ISO.NAM.005` | Case/alias collision behavior deterministic; distinct source names same ISO alias | plan/structural |
| `ISO.FILE.001` | Ordinary single extent resolves and reads correctly; first/last byte | content |
| `ISO.FILE.002` | Multi-extent chain terminates and section IDs match; truncate chain | structural |
| `ISO.FILE.003` | Multi-extent sum checked, >4 GiB represented via sections | structural/content |
| `ISO.FILE.004` | Interleaving reads units but not gap; targeted data-pattern witness | content |
| `ISO.FILE.005` | Extended attribute blocks skipped and bounded; XAR length > extent | structural |
| `ISO.FILE.006` | Zero-byte file extracts as empty and consumes no unexpected byte | content |
| `ISO.FILE.007` | ISO/SVD shared payload alias is accepted; divergent logical lengths are not | structural |
| `ISO.FILE.008` | File data may not overlap incompatible metadata allocation | structural |
| `ISO.SUSP.001` | Root `SP` check bytes and version valid; mutate BE EF | structural |
| `ISO.SUSP.002` | TLV has length ≥4 within record; zero length | structural |
| `ISO.SUSP.003` | `CE` tuple in bounds and parseable; LBA past end | structural |
| `ISO.SUSP.004` | CE continuation graph has no cycle; CE points to itself | structural |
| `ISO.SUSP.005` | Total expanded CE bytes within configured limits; amplifier chain | structural/limit |
| `ISO.SUSP.006` | `ER` identifies recognized RRIP before RR names are enabled; delete ER | structural |
| `ISO.RR.001` | `PX` length/fields valid, POSIX kind matches ISO dir flag; corrupt mode | structural |
| `ISO.RR.002` | `NM` continuation produces exact name; missing last fragment | structural |
| `ISO.RR.003` | `SL` components reconstruct exact target; dangling continue | structural |
| `ISO.RR.004` | `TF` flags and field lengths agree, valid calendar | structural |
| `ISO.RR.005` | Relocated CL/PL/RE tree is acyclic and correctly reparented | structural |
| `ISO.RR.006` | `RE` physical copy not surfaced as duplicate logical child | structural |
| `ISO.RR.007` | Hardlink identity/link count internally consistent | structural |
| `ISO.JOL.001` | SVD recognized by `%/@`, `%/C`, `%/E`; wrong escape | header |
| `ISO.JOL.002` | Regular identifiers even UCS-2BE byte length, special IDs remain 1 byte | structural |
| `ISO.JOL.003` | UCS-2 rejects surrogate / disallowed controls; crafted name | structural |
| `ISO.JOL.004` | Maximum identifier 128 bytes under Joliet; try 130 | structural |
| `ISO.JOL.005` | Joliet path table points to Joliet, not PVD, tree | structural |
| `ISO.JOL.006` | Encoded UCS-2 sort order and duplicate resolution correct | structural |
| `ISO.JOL.007` | Joliet independent tree correctly shares physical content | structural/content |
| `ISO.BOOT.001` | El Torito boot record type0 and catalog LBA valid | header/structural |
| `ISO.BOOT.002` | Validation signature `55 AA` and LE-word checksum = 0 | structural |
| `ISO.BOOT.003` | Initial boot indicator/media type/load address valid | structural |
| `ISO.BOOT.004` | Section count boundaries and EFI platform selection valid | structural |
| `ISO.BOOT.005` | Catalog and boot image extents do not exceed image | structural |
| `ISO.WRITE.001` | Output byte-identical under deterministic reordered source scans | writer |
| `ISO.WRITE.002` | Every byte of emitted image initialized, including sector padding | writer |
| `ISO.WRITE.003` | Required metadata loss fails at `plan`, optional loss counted | writer |
| `ISO.WRITE.004` | No sink writes before full plan validation; source mutation fails safely | writer |
| `ISO.WRITE.005` | ISO primary, Joliet and RR projections agree on byte content | writer/content |
| `ISO.WRITE.006` | Planned fixed image too small fails, larger exact size zero-pads | writer |

These policies are **stage-specific** and need to use the common harness's expected statuses, not a bespoke ISO exit-code mapping. E.g. invalid deep directory records can pass header scope; `verify` (structural) must reject. Invalid optional El Torito catalog may be reported separately if the chosen profile excludes boot validation; document the declared `verify` contract before encoding expected results.

### 17.3 Generator families

Make parameterized **positive** builders independent of production code:

- `minimal`: system area, PVD, terminator, L/M path tables, root directory with only `.` and `..`, zero files.
- `shallow_tree`: empty directories, one-byte files, 2047/2048/2049-byte files, 1 KiB and random data, reproducible timestamps, path names of boundary lengths.
- `large_directory`: 100–100k entries, sector-boundary padding, path-table + directory lengths at 2048 boundary, all valid sorted entries.
- `names_and_versions`: Level 1/2/3 projected aliases, upper case, blank extensions, multiple versions, associated/hidden files.
- `rr_metadata`: POSIX modes, symlinks, one 1023-byte symlink, FIFO, dates, user/group, `NM` spanning CE, long TF fields.
- `rr_relocated`: depth 9–20 logical tree via CL/PL/RE, multiple children, mixed real and moved sibling.
- `joliet`: BMP non-ASCII and ASCII, mixed case, 64-codepoint boundary, separate PVD/SVD path tables, shared data.
- `multi_extent`: >4 GiB logical length using sparse fixture in the host (no huge memory), 2+ sections, varying final tails.
- `interleaved`: unique per-block patterns, different U/G combinations and irregular tail.
- `xar`: one- and two-block XAR, time and permissions fields, ordinary payload after XAR.
- `boot_catalog`: BIOS only, EFI only, two-platform, with/without visible boot image file.
- `multisession`/`XA`/`zisofs`: authentic external fixtures to establish unsupported boundaries and planned future work.

**Mutators** are targeted byte edits to **exactly one intended invariant** when feasible. Pairwise edits are important for fields whose both-endian representation must remain equal: one mutation modifies LE half only (corrupt), a second modifies both halves while keeping equality but pointing out of range (different corrupt reason), a third changes length and relocates real payload to produce a valid witness. Mutations should be applied to private bytes, never run blindly on valuable source images.

### 17.4 Differential and metamorphic fuzzing

Create `fuzz/iso9660.c` modeled after [`fuzz/fat.c`](https://github.com/stass/fsimg/blob/master/fuzz/fat.c), feeding a bounded in-memory positional source to `probe`/`open`/`verify`/`ls`/small `cat`. Seed with tiny independent-builder images and actual external-generator images; include all namespaces and boot catalogs. Add a dedicated small SUSP/CE cursor fuzz entry point with a checked virtual block map; a name-decoder entry point for UCS-2 and ISO versions; and a writer-plan mutation/property entry point for source catalogs.

Metamorphic assertions:

- Directory entries reordered **before writer plan** produce identical final image when source semantics identical.
- Adding valid padding beyond declared volume does not change the selected directory tree.
- Select RR vs Joliet vs PVD on the same image: logical file contents for the same canonical source file match, but name/metadata may differ.
- Changing unused descriptor application bytes must not alter file content and should only affect verification if those bytes are reserved by the standard.
- Breaking one endian half of a required `733` field must be rejected irrespective of byte order of the host CPU.
- Read a file with chunks 1, 511, 512, 2047, 2048, 2049, random; concatenate and compare to full read, including sections and interleave gaps.
- Run with scratch enabled/disabled and low memory; results are semantically identical when resources suffice, otherwise only the appropriate `FSIMG_ELIMIT` differs.
- `write(A) → read_fsimg(A) → extract → create(B)` must reproduce contents and representable metadata; full binary equality requires same timestamp/order/volume-layout policy.

Always test with ASan+UBSan, `-fsanitize=fuzzer`, timeouts, cancellation, truncated sources and short injected I/O. Preserve minimal reproducible samples by hash, and record tool version/compiler/sanitizer version; treat independent-oracle disagreements as triage, **not** automatic evidence that fsimg is wrong.

## 18. Interoperability tools and concrete commands

### 18.1 Tool matrix and license hygiene

| Tool | Read/list/extract | Create ISO | Check/verify | License / role |
|---|---|---|---|---|
| **FreeBSD / NetBSD `makefs -t cd9660`** | No | **Yes** (basic ISO, Rock Ridge; platform features differ) | Writer as independent reference | BSD source; preferred permissive writer oracle. |
| **`bsdtar` / libarchive** | **Yes** ISO, Rock Ridge, Joliet | **Yes** (ISO/RR/Joliet/El Torito; depends on build) | Independent listing/extraction **and writing** oracle | BSD-2-Clause; preferred permissive reader **and writer** reference. |
| **FreeBSD/NetBSD `mount_cd9660`** | **Yes**, root/privileged usually required | No | Independent OS filesystem implementation | BSD; optional controlled/VM oracle. |
| **`xorriso` / libisofs** | Yes | **Yes** (RR, Joliet, El Torito, XA, UDF hybrid) | Yes (catalog/readback/media checks) | GPL family; **external oracle only**, never import implementation code. |
| **`isoinfo` / `isovfy` / `isodump`** | Yes (`isoinfo`) | No | `isovfy` structural checks | cdrkit/cdrtools provenance varies; external oracle only. |
| **`genisoimage` / `mkisofs`** | Not primary | **Yes** | In tandem with `isovfy` | Typically GPL/cdrtools licenses differ; external only. |
| **`pycdlib`** | **Yes** | **Yes** | API-asserted structure and attributes | LGPL-2.1-only; **external Python test process only**. |
| **`hadris-iso`** | **Yes** | **Yes** | CLI and Rust library | MIT; permissive independent extra oracle. |
| **`kdomanski/iso9660` Go package** | **Yes** | **Yes** | Programmatic fixture helper | BSD-2-Clause; lacks Joliet and has limited RRIP: use for base ISO only. |
| **macOS `hdiutil`** | Attach/list mounted ISO | No ISO creation guarantee | System interoperability | Platform tool; optional macOS integration. |
| **Linux kernel `mount -t iso9660`** | Read/list | No | Kernel read oracle | GPL kernel, **runtime test only** in disposable VM. |
| **7-Zip (`7z l/x`)** | Read/extract many ISO images | No general ISO writer | Independent extra listing | Check package license; external tool only. |
| **QEMU + OVMF/SeaBIOS** | Runtime CD boot | No | Boot smoke tests (not structural proof) | External test system, run controlled images. |

The tool matrix is deliberately **asymmetric**: an ISO creator and an ISO parser must be independent. It is better to compare three structurally different implementations than to build and validate using one library. `libarchive` has **both a reader and an ISO writer**; `bsdtar --format=iso9660` can be used as a creator when the installed build supports it. However, its own reader and writer share a codebase, so count them as *one implementation family*, not two independent families.

### 18.2 Writer-generated images, independently consumed

Commands below are shell snippets for systems where each program is installed. `fsimg` commands are **proposed** for the ISO format after it is implemented; existing `fsimg create` flags are retained. `-R` is **fsimg's reproducible-image option**, **not** its Rock Ridge extension option. For ext selection, use the existing `-O key=value` format-specific option surface after adding ISO keys.

```sh
mkdir -p /tmp/fsimg-iso-input/dir
printf 'hello\n' >/tmp/fsimg-iso-input/HELLO.TXT
printf 'content-2' >/tmp/fsimg-iso-input/dir/other.txt

# Proposed fsimg ISO writer; keys are suggested, NOT yet supported.
fsimg create -R -t iso9660 -o /tmp/fsimg.iso \
  -O iso-level=2 -O rockridge=on -O joliet=on \
  /tmp/fsimg-iso-input

# Inspect with independent implementations:
isoinfo -d -i /tmp/fsimg.iso
isoinfo -i /tmp/fsimg.iso -f                     # ISO primary names
isoinfo -i /tmp/fsimg.iso -R -f                  # RR names when present
isoinfo -i /tmp/fsimg.iso -J -f                  # Joliet names when present
isoinfo -i /tmp/fsimg.iso -p                     # path table
isovfy /tmp/fsimg.iso
bsdtar -tf /tmp/fsimg.iso
mkdir -p /tmp/fsimg-iso-extracted
bsdtar -xf /tmp/fsimg.iso -C /tmp/fsimg-iso-extracted
xorriso -indev /tmp/fsimg.iso -ls_r /
```

**Note:** Some `bsdtar` versions prefer RR and others Joliet based on library options; use `bsdtar --options iso9660:!rockridge` and `iso9660:!joliet` where available to isolate a namespace, as documented in [`bsdtar(1)`](https://github.com/libarchive/libarchive/blob/master/tar/bsdtar.1). External names may be case-normalized by a specific tool; comparison should use selected view and record which one was observed.

### 18.3 Independently generated images, read by fsimg

```sh
# BSD makefs (exact option set varies by installed makefs build):
makefs -t cd9660 -o rockridge -o volumeid=FSIMGTEST \
  /tmp/makefs.iso /tmp/fsimg-iso-input

# GNU xorriso acting as a mkisofs-compatible creator:
xorriso -as mkisofs -iso-level 2 -R -J -V FSIMGTEST \
  -o /tmp/xorriso.iso /tmp/fsimg-iso-input

# Optional legacy creator (external oracle only):
genisoimage -iso-level 1 -R -J -V FSIMGTEST \
  -o /tmp/geniso.iso /tmp/fsimg-iso-input

# Portable BSD-licensed ISO writer (if the bsdtar build enables the format):
bsdtar -cf /tmp/libarchive.iso --format=iso9660 \
  --options iso9660:rockridge,iso9660:joliet -C /tmp/fsimg-iso-input .

# Proposed reader: base, RRIP, Joliet namespaces and structural verify.
for name in /tmp/makefs.iso /tmp/xorriso.iso /tmp/geniso.iso /tmp/libarchive.iso; do
    fsimg info -i "$name"
    fsimg ls -r -i "$name"
    fsimg verify -d -i "$name"
    fsimg ls -i "$name" -O namespace=iso
    fsimg ls -i "$name" -O namespace=rr
    fsimg ls -i "$name" -O namespace=joliet
done
```

Because `makefs` output capabilities vary by BSD, do not assert it creates Joliet without checking its installed manual. `genisoimage` and `xorriso` are useful **generators** even though their source licenses do not qualify for inclusion in `fsimg`. `pycdlib` provides yet another writer and a convenient programmatic way to make unusual tree/boot combinations. For >4 GiB fixtures, use real sparse files on an FS with sparse-file support and budget disk/output explicitly; an ISO image may still physically occupy many gigabytes even if its backing storage is sparse.

### 18.4 El Torito independent tests

```sh
# Assuming source tree already contains boot/bios.img with valid boot program:
xorriso -as mkisofs -R -J \
  -c boot/boot.cat -b boot/bios.img \
  -no-emul-boot -boot-load-size 4 \
  -o /tmp/bios-boot.iso /tmp/fsimg-boot-source

# Catalog layout / boot equipment inspection:
xorriso -indev /tmp/bios-boot.iso -report_el_torito plain
isoinfo -d -i /tmp/bios-boot.iso
isovfy /tmp/bios-boot.iso
```

For EFI fixtures, provide a *valid EFI FAT image* as a source and create an El Torito platform `0xEF` entry (for `xorriso -as mkisofs`, commonly `-eltorito-alt-boot -e path/to/efiboot.img -no-emul-boot`; verify syntax against the installed version). Validate catalog via `fsimg verify` and QEMU+OVMF only as a **separate** boot test. Firmware boot success tests the boot image too; `fsimg` must not claim to validate bootloader executable logic.

### 18.5 Independent contents and metadata comparisons

The definitive interoperability test uses a manifest produced *before* generating any ISO image:

```text
path_utf8, entry_kind, logical_size, sha256(file_contents),
link_target_bytes, modified_time, mode, uid, gid
```

Compare the manifest against each reader's extract/list results in the **same selected namespace**; map ISO projected aliases via a separate deterministic source-to-namespace manifest. For Joliet, do not assume arbitrarily non-BMP names survive; for RRIP, don't expect ordinary `bsdtar` to perfectly preserve ownership on an unprivileged host. Prefer compare *reported metadata* over extracted owner bits that the host cannot set. `sha256sum` or a Ruby hash is a test harness dependency only, not a runtime dependency.

Use *both directions*:

1. `fsimg` writer → `isovfy` / `isoinfo` / `bsdtar` / `xorriso` / OS mount / `pycdlib` reader.
2. `makefs` / `xorriso` / `genisoimage` / `pycdlib` writer → `fsimg` `info`, `ls`, `cat`, `extract`, structural `verify`.
3. Verify each product with a raw-byte independent Ruby decoder that does **not** call fsimg and **does not** trust external tools' status codes as proof of correctness.
4. Record tools' exit statuses, warning stderr, exact version and mismatches, and mark unsupported tool-feature combinations (e.g. lack of Joliet in `makefs`) as skip, never fail or spurious pass.


---

## 19. Conformance, interoperability and regression matrix

The matrix below is a **minimum release requirement**, not a claim that `fsimg` already passes. A case is accepted only when an independently generated fixture demonstrates the stated feature, `fsimg` reports the expected namespace/metadata/payload, and at least one independently implemented consumer accepts any image generated by `fsimg`. All test fixtures have an explicit manifest containing source tool, version, invocation, origin, hash, expected profile and expected policy. Never let a creator's own reader be the only oracle.

| Case family | Reader fixture creation | `fsimg` expected observation | Independent writer-output validation |
|---|---|---|---|
| Minimal base ISO | `makefs`, libarchive writer, `xorriso` | PVD, root self/parent, one file, correct bytes | `isovfy`, `isoinfo`, `bsdtar`, `pycdlib` |
| Empty tree, zero files | All three permissive writers where practical | Root and required directory records only | OS mount, `isovfy`, raw parser |
| ISO Level 1 | `xorriso -iso-level 1`; libarchive `iso-level=1` | Correct 8.3/name/version rules | `isoinfo -f`, OS mount |
| ISO Level 2 | `makefs`, `xorriso -iso-level 2` | Longer primary identifiers within profile | `isoinfo -f`, filename namespace audit |
| Multiple file versions | Synthetic ISO and third-party tools allowing `;N` | Deliberate version listing and selection | Namespace-key regression model |
| Path-table parity | All compatible writers | Type L/M, matching BFS hierarchy and parents | Independent Ruby parser, `isoinfo -p` |
| RRIP basic names/mode | `makefs -o rockridge`; `xorriso -R`; libarchive writer | Primary aliases; RR names, types, uid/gid/mode | `bsdtar -tvf`, BSD mount, `isoinfo -R` |
| RRIP symlinks, FIFO | `makefs` / libarchive writer / `xorriso -R` | Correct target/type; no link following | POSIX-lstat manifest and metadata comparison |
| RRIP `CE` continuation | Writer with long names or generated TLV fixtures | Bounded continuation parsing; exact names | `isoinfo -R`, libarchive reader, raw parser |
| RRIP deep relocation | Author image with >8 logical levels | Resolve logical hierarchy without `RR_MOVED` duplication | BSD/Linux mount, libarchive where compatible |
| Joliet level 1/2/3 | `xorriso -J`, libarchive writer, `pycdlib` | UCS-2BE -> UTF-8, independent SVD hierarchy | `isoinfo -J`, Windows mount where available |
| ISO+RR+Joliet combined | `xorriso -R -J`, libarchive writer | Selected names stay within selected hierarchy | Explicit three-namespace listing/digest compare |
| Unicode boundary | Controlled names: Greek/CJK, Latin combining, non-BMP | Correct representable Unicode; explicit reject/loss for unrepresentable | Joliet raw 2-byte units and `pycdlib` |
| Identifier collisions | Synthetic and BSD `makefs` collision regression | Full-directory ambiguity validation | Unique `isoinfo` projected alias manifest |
| Multi-extent >4 GiB | Sparse source + `xorriso -iso-level 3`, synthetic image | One entry, 64-bit logical length, correct slices | Digest of beginning/boundary/end; OS reader if available |
| Interleave / XAR | Synthetic byte-accurate fixture | Read correct logical data or explicit unsupported | Ruby geometric address oracle |
| El Torito single BIOS entry | `xorriso -as mkisofs -b ...`, libarchive ISO writer | Valid descriptor, checksum, correct LBA/count | `xorriso -report_el_torito`, QEMU/SeaBIOS optional |
| El Torito EFI and multi-platform | `xorriso` with BIOS+EFI section | Parse catalog platform sections and checked count | `xorriso -report_el_torito`, OVMF optional |
| `zisofs` | `xorriso`/libarchive with zisofs enabled | Either correct uncompressed bytes or unsupported | `bsdtar`, Linux kernel if zisofs configured |
| Raw BIN/CUE mode image | CUE producer, known offset sectors | Clear unsupported/raw-adapter policy, not false success | `bchunk`/`cdrdao` adapter and raw frame model |
| ISO/UDF bridge | DVD/UDF authoring tool | Valid ISO namespace if present; do not conflate trees | `xorriso`, OS mount under selected FS type |
| Reproducibility | Twice create same canonical manifest | Byte-for-byte identical image with `-R` | SHA-256 equality; no uncontrolled timestamps/ordering |
| Corrupted descriptor/endian | Ruby byte mutation | Header/structural failure according to rule scope | Independent raw-decoder report |
| Corrupted RRIP `CE` cycle | Ruby mutate continuation pointers | Bounded `FSIMG_ECORRUPT`; never unbounded work | ASan/UBSan and timeout |
| Truncated source mid-read | Faulted `fsimg_io_t` callback | Precise I/O state, no fabricated success | Fault-injection C harness |
| Extraction traversal attack | ISO/RR generated `..`, separator, symlink pivot | No escape, safe skip/refusal, no host writes outside root | File-system boundary/inode checks |
| Memory/fd/scratch stress | Huge directory / deep tree / many extents | Bounded memory, scratch consumption and fd count | Quota and RSS/FD observability harness |

### 19.1 Scopes and failure locations

For a negative test classify **where** a valid image becomes known invalid:

- **Probe/open**: sufficient identity, descriptor framing and unsupported mandatory geometry. The probe is a pure bounded-prefix operation; a PVD that can exist after the chosen prefix bound is handled as an explicit support policy, not guessed from arbitrary bytes.
- **Directory preparation**: framing, field encodings, names, RRIP/Joliet consistency and collisions; no child can be exposed until sibling keys are validated.
- **Selected-file read**: actual data extents and `resolve()` bounds, including multi-extent and interleave transitions; an off-branch file can remain unverified until structural verify.
- **Complete verification**: all reachable directories, contents and permitted extents, directory allocations, metadata dependencies, boot catalog, path-table cross-checks, optional payload read-through.
- **Writer plan**: capability, fidelity, identifier mapping, per-record packing, size and LBA representability. No sink bytes are emitted on plan failure.
- **Extraction**: host path safety, transactional private destination, link/FIFO policy, host collisions and quotas.

A test model may mark later scopes **unclassified** when a malformed preceding record prevents safe decoding. Such a case is not a pretext for unpredictable runtime behavior: the parser must still reject or report unsupported under its advertised scope and limits.

### 19.2 Regression corpus design

Maintain a small permanent deterministic corpus and a large generated suite. A useful layout compatible with `test/spec/` is:

```text
test/spec/lib/fsimg_spec/iso9660/
    bases.rb                 # independent base image recipes
    layout.rb                # raw numeric codecs and directory-record framing
    volume_descriptors.rb    # PVD/SVD/EVD/catalog decoders
    directory.rb             # path tables and names
    susp.rb                  # independent bounded tagged/TLV decoder
    rock_ridge.rb            # PX/NM/SL/TF/CE/CL/PL/RE model
    joliet.rb                # UCS-2BE codec and namespace policy
    el_torito.rb             # boot descriptor, validation and entry model
    rules.rb                 # named test predicates, rejection policies
    witnesses.rb             # valid and violating boundary witnesses
    cases.rb                 # recipe selection/target mutations
    suite.rb                 # integration with generic runner
    toolchain.rb             # external oracle adapters

test/spec/cases/iso9660/
    ...                       # promoted minimal regressions

test/iso9660.c               # internal white-box and contract tests
fuzz/iso9660.c               # standalone image parser fuzz target
fuzz/iso9660_susp.c          # TLV/CE and deep-relocation parser target
fuzz/iso9660_directory.c     # directory record and name parser target
```

Treat these filenames as proposed modules rather than existing paths. Their semantics follow the already established Ruby harness contract: BinData for layout, pure Ruby predicates for constraints, source-independent witness generation, separate result classification and replay. Never copy an implementation's decoder logic into the oracle; a highly correlated shared bug is worse than an oracle that skips an unsupported feature.

### 19.3 High-yield interoperability failures to promote

Particularly valuable cases are the following: a creator truncates names before checking collisions; an empty directory's padding is mistaken for a zero-length entry; a Joliet decoder mistakenly decodes `00 00` as a self-entry; source file bytes cross into a `CE` continuation area; RR `CL` resolves through a physical relocation cycle; RR `SL` continuation loses a slash; double-endian fields disagree; version handling merges distinct `;1` and `;2` files; multiple file sections are counted as separate files; an EFI boot image is truncated using a BIOS 512-byte load-sector count; a type-2 enhanced descriptor is misidentified as Joliet; `CD001` in random payload is accepted as a volume; and an early boot-record descriptor causes a signature-only probe to miss a later PVD.

The FreeBSD `makefs` regression for duplicate **truncated directory names** is an especially useful case to reproduce; the historical fix and ATF test are recorded in [FreeBSD review D48251](https://reviews.freebsd.org/D48251). It illustrates why `fsimg` must validate and, when writing, generate **collision-free projected ISO identifiers** independently of Rock Ridge spelling.

---

## 20. Delivery milestones and acceptance gates

The priority ordering minimizes the amount of shared `fsimg` architecture that needs changing. Do not implement a new general-purpose optical abstraction first: start with the simple descriptor and let more elaborate extension requirements validate the abstractions.

| Stage | Deliverables | Exit gate |
|---|---|---|
| **M0 — format skeleton and probe** | Static `fsimg_iso9660_format` descriptor; detect PVD after possible type-0 boot descriptor; basic label/geometry; reliable `EFORMAT/ECORRUPT/EUNSUPPORTED`. | Read minimal images from `makefs`, libarchive and `xorriso`; negative descriptor table passes. |
| **M1 — plain reader** | PVD tree, path-table checks, `prepare/replay/keys/key/resolve`; ISO identifier/version handling, no XAR/interleave; `verify`. | Every plain fixture can be `info`/`ls`/`cat`/`extract`/`verify`ed; no unbounded allocation. |
| **M2 — plain writer** | Deterministic primary names, zero-filled sectors, complete PVD and path tables, planned metadata/data regions, direct positional source copying. | `isovfy` and at least two independent readers accept output; byte reproducibility. |
| **M3 — RRIP** | SUSP stream, CE cycles, NM/SL/PX/TF/PN types, RR metadata and POSIX names, writer SUSP packing. | RRIP names and metadata round-trip through BSD/Linux reader and an external creator; malformed-TLV corpus passes. |
| **M4 — Joliet** | Separate UCS-2BE tree, descriptor, path tables, deterministic names; reader namespace selection; writer shared payload. | Primary/RR/Joliet triple-view tests and Unicode boundaries pass. |
| **M5 — deep RR + Level 3** | `CL/PL/RE` relocation and `RR_MOVED` filtering; 64-bit multi-extent reader/writer. | Deep-tree and >4 GiB fixtures pass, bounded indexes demonstrated. |
| **M6 — El Torito** | Boot-catalog verification; boot-image input/build path and platform sections; data-layout checks. | BIOS and EFI catalogs inspect correctly in `xorriso`; QEMU smoke tests where a valid bootloader is supplied. |
| **M7 — optional variants** | Enhanced (version 2), XAR, interleaving, multisession explicit selection; later zisofs/AAIP/XA. | Each profile independently qualified, unsupported variants remain explicit. |

A stage is **not** done until the following all hold: one independently authored C-level test set; one independent Ruby specification rule set; at least two read-oracle families and one write-oracle family where the stage writes images; ASan + UBSan runs; fuzz seeds seeded from both valid and invalid images; bounded memory/FD/scratch tests; deterministic output comparison; stable reference/license manifest; and a human-readable summary of supported and explicitly unsupported ISO variants.

### 20.1 Proposed driver file/module boundaries

```text
lib/formats/iso9660/
    iso9660.h             # driver state, checked geometry, internal API
    descriptor.c          # probe, capability/profile records
    codec.c               # 711/721/723/731/732/733 and timestamp codecs
    volume.c              # PVD/SVD/EVD enumeration and namespace selection
    path-table.c          # primary/alternate path tables
    directory.c           # byte framing, directory preparation and replay
    names.c               # ISO aliases, versions, Joliet Unicode names
    susp.c                # generic SUSP, CE and bounded continuation
    rockridge.c           # NM, PX, TF, SL, CL/PL/RE
    content.c             # multi-extent and interleave -> resolve()
    verify.c              # structural claims and cross-checks
    eltorito.c            # boot-record and catalog parser
    write-plan.c          # layout iteration and capacity checks
    write-names.c         # alias projections, directory sorting
    write-directory.c     # directory-record and SUSP packing
    write-emit.c          # descriptor, paths, dirs, data and padding
    write-boot.c          # explicit optional boot image/catalog output
```

This is an **organizational suggestion**: combine smaller modules when simplicity improves, rather than mechanically introducing every file. Follow existing `lib/formats/fat/`, `lib/formats/exfat/` and `lib/formats/qnx6/` format patterns. Read current project `CLAUDE.md` and `specs/style.md` before changes. Descriptors remain the only registry interface. Implementation modules remain private C11 and use `_t` typedef names as specified in the style guide.

### 20.2 Suggested CLI format settings, not existing flags

Current CLI already supports `-t format` and typed `-O key=value` dispatch for recognized format-specific options; do not invent an independent ISO command syntax. Proposed new ISO writer keys:

| Key | Example | Policy |
|---|---|---|
| `iso-level` | `1`, `2`, `3` | Primary hierarchy legality; 3 permits multi-extent. |
| `rockridge` | `off`, `on` | POSIX namespace emission into primary record SUSP. |
| `joliet` | `off`, `1`, `2`, `3` | Enable SVD and select UCS-2 escape profile. |
| `relocate` | `on`, `off` | Whether to relocate deep physical ISO directories for RRIP. |
| `boot-catalog` | image-relative path | Requires matching boot-source API/source entry; no arbitrary host pathname implied. |
| `boot-bios`, `boot-efi` | source reference | Explicit boot stream and declared platform fields, not magic path guessing. |
| `volume-id` | ASCII label | Maps to PVD volume identifier; generic `label` can remain canonical. |
| `pad` | `on`, `off` | Extra optical trailing sectors; always deterministic. |

Proposed reader keys: `namespace=auto|iso|rockridge|joliet|enhanced`, `version=keep|strip-single` and `session=<explicit-index>` only when session metadata exists. The default `auto` prefers a **complete, validated RRIP view**, then a complete Joliet view, then a coherent PVD view, but the precise preference should be documented and tested. This is a proposed product choice, *not* a claim that the existing CLI supports these keys. Generic `-O` CLI parser can be wired into a properly versioned C library option surface.

**Avoid option overload.** `-R` in the current `fsimg` CLI means **reproducible**; it must not become the `mkisofs`/`isoinfo` convention for Rock Ridge. `-b` currently selects the input image **byte offset**, not the El Torito boot image as it does in some creators.

---

## 21. Primary sources, permissively licensed implementation references and external tools

This section is a **license-aware reading list**, not a code-import authorization. License classifications are based on the described upstream project/file and should be verified again at the exact commit used. A BSD/MIT project may contain separately licensed assets, generated files, vendor code or test images. No external project is needed as a runtime dependency of `fsimg`.

### 21.1 Standards, specifications, format descriptions (factual/reference use)

| Source | Link | Why it matters / caveat |
|---|---|---|
| **ECMA-119, 6th edition (Dec 2025)** | [Official ECMA landing page](https://ecma-international.org/publications-and-standards/standards/ecma-119/) · [Official PDF](https://ecma-international.org/wp-content/uploads/ECMA-119_6th_edition_december_2025.pdf) | **Primary normative source** for volume/file structures and current editions, Annex C for Joliet. Public availability is *not* permission to relicense copied prose. |
| **ECMA-119, 2nd edition (Dec 1987)** | [Official archived PDF](https://www.ecma-international.org/wp-content/uploads/ECMA-119_2nd_edition_december_1987.pdf) | Historical 1988-era baseline; useful for distinguishing modern revisions and enhanced behavior. |
| **IEEE P1281 SUSP, draft 1.12 (1994)** | [Public reading mirror](https://studylib.net/doc/18849138/ieee-p1281-system-use-sharing-protocol-draft) · [Archived historical PDF locator](https://web.archive.org/web/20170404132301/http://www.ymi.com/ymi/sites/default/files/pdf/Systems%20Use%20P1281.pdf) | `SP`, `CE`, `ST`, `PD`, `ER`, continuation and extension selection. Copyrighted draft; do not transplant prose or diagrams. Historical `ymi.com` URLs may be unavailable today. |
| **IEEE P1282 RRIP, draft 1.12 (1994)** | [PDF preserved on FreeBSD server](https://people.freebsd.org/~emaste/rrip112.pdf) | `PX`, `PN`, `SL`, `NM`, `CL`, `PL`, `RE`, `TF`, `SF`. A draft and copyrighted reference, not permissive code. |
| **Joliet specification (1995)** | [Official ECMA-119 Annex C](https://ecma-international.org/wp-content/uploads/ECMA-119_6th_edition_december_2025.pdf) · [historical text mirror](https://pismotec.com/cfs/jolspec.html) | UCS-2 character levels and path/filename rules, separate SVD hierarchy. Prefer official ECMA annex. |
| **El Torito 1.0 bootable CD-ROM format** | [Historical PDF](https://misc.manty.net/eltorito.pdf) | Boot descriptor, catalog validation, 32-byte entries, platform and media flags; proprietary rights on *document text* remain. |
| **UEFI specification** | [UEFI 2.10 media protocols](https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html) · [UEFI specification index](https://uefi.org/specifications) | UEFI optical boot considerations, EFI platform code and fallback semantics; check current revision for firmware acceptance testing. |
| **ECMA-167 / UDF background** | [ECMA-167 listing](https://ecma-international.org/publications-and-standards/standards/ecma-167/) · [OSTA UDF resources](https://www.osta.org/specs/) | Clearly distinguishes UDF descriptor systems from ISO primary/SVD hierarchies and UDF/ISO bridges. |
| **CD-ROM XA and optical sector background** | [Linux CD-ROM documentation](https://docs.kernel.org/cdrom/index.html) · [libcdio documentation](https://libcdio.github.io/libcdio.html) | Logical vs raw sectors, Mode 1/2, cue/transport separation. GPL library code is **not** a code donor. |
| **zisofs explanatory format** | [Kaitai format gallery](https://formats.kaitai.io/zisofs/) | Compressed-file header/block mapping; check description/test-data provenance before reuse. |
| **AAIP extension descriptions** | [libisofs AAIP descriptive material](https://fossies.org/linux/xorriso/doc/susp_aaip_2_0.txt) | Separate extension from RRIP, applicable only if additional metadata is desired. Not a permissive implementation. |
| **libarchive ISO format wiki** | [FormatISO9660](https://github.com/libarchive/libarchive/wiki/FormatISO9660) | Implementation-facing overview of ISO reading **and writing**, and explicit list of writer extension options. BSD code below is a better implementation reference. |

### 21.2 Permissively licensed reader, writer and test code

| Codebase / file | Link | License and practical use |
|---|---|---|
| **FreeBSD kernel `cd9660`** | [Source tree](https://github.com/freebsd/freebsd-src/tree/main/sys/fs/cd9660) · [iso.h](https://github.com/freebsd/freebsd-src/blob/main/sys/fs/cd9660/iso.h) · [Rock Ridge reader](https://github.com/freebsd/freebsd-src/blob/main/sys/fs/cd9660/cd9660_rrip.c) | BSD-family file licenses. Excellent mature reader, numeric/header definitions, directory traversal, RRIP edge cases. Inspect each file's header. |
| **FreeBSD `makefs` ISO writer** | [Writer source tree](https://github.com/freebsd/freebsd-src/tree/main/usr.sbin/makefs) · [`cd9660.c`](https://github.com/freebsd/freebsd-src/blob/main/usr.sbin/makefs/cd9660.c) · [ISO subdirectory](https://github.com/freebsd/freebsd-src/tree/main/usr.sbin/makefs/cd9660) | BSD-family, excellent **image creator** reference for directory planning, RRIP, aliases and El Torito options. Existing tests may expose known limitations. |
| **FreeBSD makefs regression tests** | [`makefs_cd9660_tests.sh`](https://github.com/freebsd/freebsd-src/blob/main/usr.sbin/makefs/tests/makefs_cd9660_tests.sh) · [2024–25 collision fix](https://reviews.freebsd.org/D48251) | BSD test patterns, deliberately tricky name collision, device-node/Rock Ridge coverage. Adapt *test ideas* first; don't make `fsimg`'s independent oracle a clone of tests it borrowed. |
| **NetBSD `makefs` and ISO writer** | [NetBSD `makefs` tree](https://github.com/NetBSD/src/tree/trunk/usr.sbin/makefs) | BSD-family permissive, useful second implementation lineage/cross-portability review; some history shared with FreeBSD, so not statistically independent from it. |
| **OpenBSD `makefs`** | [Manual](https://man.openbsd.org/makefs.8) · [OpenBSD src repo](https://github.com/openbsd/src) | BSD-family, optional alternate writer; check availability of `cd9660` extension flags in installed version. |
| **libarchive ISO reader** | [`archive_read_support_format_iso9660.c`](https://github.com/libarchive/libarchive/blob/master/libarchive/archive_read_support_format_iso9660.c) | BSD-2-Clause upstream reader; specifically covers Joliet, RRIP, zisofs and defensive extraction. |
| **libarchive ISO **writer** | [`archive_write_set_format_iso9660.c`](https://github.com/libarchive/libarchive/blob/master/libarchive/archive_write_set_format_iso9660.c) | **BSD-2-Clause, highly relevant creator**: ISO levels, Rock Ridge, Joliet, El Torito, optional zisofs. The internal temporary-payload-file strategy is intentionally different from `fsimg`'s direct two-pass source streaming; study without inheriting that requirement. |
| **libarchive ISO regression tests** | [Reader/writer test directory](https://github.com/libarchive/libarchive/tree/master/libarchive/test) · [`test_write_format_iso9660_filename.c`](https://github.com/libarchive/libarchive/blob/master/libarchive/test/test_write_format_iso9660_filename.c) · [`test_read_format_isojoliet_versioned.c`](https://github.com/libarchive/libarchive/blob/master/libarchive/test/test_read_format_isojoliet_versioned.c) | BSD-2-Clause test ideas/fixtures: levels 1–4, long Joliet names, ISO alias projection, versioned Joliet reading. Check test-asset provenance separately. |
| **Go ISO9660 module, kdomanski** | [kdomanski/iso9660](https://github.com/kdomanski/iso9660) | BSD-2-Clause. Simple self-contained parser and writer; useful for clean independent fixture generation. Confirm RRIP/Joliet feature limits. |
| **hadris ISO Rust library** | [hxyulin/hadris](https://github.com/hxyulin/hadris) | MIT-licensed project, multi-extension ISO reader/writer including El Torito; independently useful cross-language API and corpus ideas. Inspect license on the specific crate. |
| **edk2 El Torito structures** | [`ElTorito.h`](https://github.com/tianocore/edk2/blob/master/MdePkg/Include/IndustryStandard/ElTorito.h) | `BSD-2-Clause-Patent`; small, clear industry record descriptions. Separate UEFI firmware logic and CD catalog parser. |
| **Cross-platform `makefs` fork** | [kusumi/makefs](https://github.com/kusumi/makefs) | BSD-2-Clause project intended for Linux and BSD. Useful for running a BSD ISO creator on non-BSD CI; provenance may be shared with FreeBSD `makefs`, so don't count it as fully independent. |

**Why libarchive deserves special attention:** the implementation is already BSD-compatible and is directly relevant to the original stream-vs-filesystem motivation for `fsimg`. `archive_write_set_format_iso9660()` demonstrates both why ISO is a layout-first format (directories precede content in conventional optimized images) and a common workaround: stage file payloads in temporary storage. `fsimg` can do better for its intended use case by planning against source files once and then reading them directly by positional I/O, without staging *payload* into a second complete copy. In exchange, `fsimg` must enforce source identity/snapshot constraints across its plan/emit operations.

### 21.3 External reference implementations and operational tools — do not import source

| Project/tool | Link | Practical role / license boundary |
|---|---|---|
| **GNU xorriso** | [GNU project](https://www.gnu.org/software/xorriso/) · [manual](https://manpages.debian.org/testing/xorriso/xorriso.1.en.html) | Powerful read/write and El Torito diagnosis; GPL family, **external tool only**. |
| **libisofs** | [libburnia project](https://dev.lovelyhq.com/libburnia/libisofs) | Core authoring implementation with RRIP/AAIP/Joliet/zisofs expertise; GPL-family code, **reference behavior only**. |
| **mkisofs / genisoimage / cdrkit** | [genisoimage manual](https://manpages.debian.org/trixie/genisoimage/genisoimage.1.en.html) | Widely used independent ISO image creator and regression oracle; historically varied copyright/license lines, so treat solely as external executable. |
| **isoinfo / isovfy / isodump** | [`isoinfo` manual](https://manpages.debian.org/trixie/genisoimage/isoinfo.1.en.html) · [`isovfy` manual](https://manpages.ubuntu.com/manpages/noble/man1/isovfy.1.html) | Inspect PVD, path table, filenames, raw bytes and structural warnings; do not regard one successful exit as proof of conformance. |
| **pycdlib** | [GitHub](https://github.com/clalancette/pycdlib) · [documentation](https://clalancette.github.io/pycdlib/) | LGPL-2.1-only; useful external Python fixture producer/reader via subprocess. Do not embed its source into `fsimg` or its independent Ruby oracle. |
| **Linux ISO reader** | [Kernel `fs/isofs`](https://github.com/torvalds/linux/tree/master/fs/isofs) | GPL-2.0-only kernel code; ideal independent runtime mount test on disposable VM, not a donor. |
| **GNU libcdio** | [Documentation](https://libcdio.github.io/libcdio.html) | CD image/drive read tooling and raw-sector questions; GPL-family, reference/external only. |
| **7-Zip** | [7-Zip project](https://www.7-zip.org/) | Additional ISO listing/extraction oracle, check bundled binary/runtime licenses per platform. |
| **QEMU** | [QEMU docs](https://www.qemu.org/docs/master/) | Boot catalog smoke tests using `-cdrom` plus BIOS/OVMF; not a filesystem verification oracle. |
| **SeaBIOS** | [Source](https://github.com/coreboot/seabios) | BIOS boot interpretation; licensing separate, firmware for external tests. |
| **OVMF / EDK II** | [edk2](https://github.com/tianocore/edk2) | UEFI optical boot runtime; the individual El Torito header above is permissive. |
| **isomaster** | [ISO Master project](https://littlesvr.ca/isomaster/) | GUI author/edit behavior and unusual fixtures; licenses differ from permissive donor policy. |
| **bchunk / cdrdao** | [bchunk project](https://github.com/extramaster/bchunk) · [cdrdao project](https://cdrdao.sourceforge.net/) | Convert/understand BIN/CUE/TOC and raw track addressing; external preprocessing only, not equivalent to cooked ISO parser. |
| **FreeBSD manpages** | [`makefs(8)`](https://man.freebsd.org/cgi/man.cgi?query=makefs&sektion=8) · [`mount_cd9660(8)`](https://man.freebsd.org/cgi/man.cgi?query=mount_cd9660&sektion=8) · [`bsdtar(1)`](https://man.freebsd.org/cgi/man.cgi?query=bsdtar&sektion=1) | Authoring, kernel-mount and libarchive operation syntax. Tool availability and option compatibility are platform/build specific. |

### 21.4 What may and may not enter the `fsimg` repository

**Appropriate:** original prose/tables and clean-room-style C11 written from format facts; independently authored field parsers and test rules; pointers to BSD/MIT implementations; licensed BSD/MIT code adapted with the file's required notices intact and a provenance entry in `docs/references.md`; generated test images where their source, copyright and redistribution terms permit inclusion; running GPL/LGPL tools in CI as standalone programs.

**Not appropriate under this task's constraint:** copying GPL/LGPL or license-ambiguous source into the main repo; translating copyleft code line by line into C; copying paragraphs/diagrams from proprietary standards; importing binary test assets without origin/license records; silently treating a permissive repository as permission for all embedded dependencies; or treating a locally installed proprietary program as a library dependency.

**Test license audit:** record at least URL, upstream project, branch/tag+commit, exact path, SPDX/license of the *file*, whether any exact source was adapted, copyright notices, modification summary, fixture-generation command and redistribution rights. Because test fixtures can themselves contain licensed boot loaders, screenshots, recorded audio, or OS images, default to **self-generated empty/test payloads** for all distributed corpus images and keep proprietary optical media in non-distributed local tests only.

---

## 22. Open design questions and resolution checklist

These are decisions for maintainers, not ambiguities to resolve by silent guesses inside a parser. The choices below are the **recommended defaults** for `fsimg` and are ready to be converted into explicit tests.

| Question | Recommended decision | Reason |
|---|---|---|
| How to expose the format? | One `iso9660` descriptor and unified capability; typed `-O` namespace/extension settings. | Fits static registry and avoids a public variant per extension combination. |
| Preferred default read namespace? | Valid RRIP, otherwise Joliet, otherwise ISO primary. Forced preference refuses unsupported or invalid required metadata. | Preserves POSIX semantics on Unix while offering Unicode names elsewhere; never mixes trees. |
| Should missing RRIP metadata fall back? | Only select fallback *hierarchy as a whole* in `auto`, never field-by-field across unrelated records. | Prevents silently fabricated POSIX ownership or cross-tree collisions. |
| Does `fsimg` need directory versions? | Surface distinct versions by default; optionally strip a unique `;1` in documented compatibility mode. | The names remain unambiguous and lossless under strict verification. |
| Should the reader accept ISO-1999-style EVD? | Read-only explicit enhanced profile after basic Joliet; no automatic reinterpretation of unknown type-2 descriptors. | EVD and Joliet have different encoding and naming rules. |
| How to handle non-UTF-8 RRIP names? | Reject forced RRIP view as `EUNSUPPORTED`; allow auto-select of coherent Joliet or PVD if valid. | Public `fsimg_entry_t.path` is UTF-8; no reversible byte-name escape convention exists. |
| How to handle multiple PVDs or Joliet SVDs? | Select only if unambiguous under configured profile; validate all descriptor framing/required fields. | Avoid hidden nondeterminism. |
| Need very large descriptor probe? | Retain finite default prefix and explicit unsupported result for out-of-profile descriptor sequence; design a bounded second read only if fixtures demand it. | Avoid expanding common probe memory for rare optical images. |
| How deep should writer go? | Conforming primary tree for default; RRIP relocation optional advanced support; fail early if impossible without relocation. | Better to fail cleanly than output quietly noncompliant depth. |
| How to pack RRIP `CE` data? | Deterministic continuation arena, fixed ordering, no shared/overlapping fragments unless deliberate. | Simplifies writer plan and structural verifier. |
| Should hardlinks share data? | Reader accepts consistent RRIP hardlinks; writer can coalesce only when source identity is explicit, safe and reproducible. | Plain ISO duplicate extents are often legal cross-namespace aliases, so generic duplicate-storage rejection is wrong. |
| Must writer support multi-extent at launch? | Reader P1, writer after interoperability gates; the initial writer rejects >single-section file-size limit. | Most small ISOs are served without introducing an extent index immediately. |
| How to represent boot entries publicly? | Optional versioned read-only boot-info API, not directory entries or fake files. | El Torito catalogs are not necessarily in filesystem hierarchy. |
| Is a raw 2352-byte BIN/CUE input in scope? | No for v1; separate explicit transport adapter with verified Mode1/Mode2 conversion. | Do not confuse track-addressing and logical ISO addressing. |
| Are images writable in place? | No; creation-only writer and reader. | Matches `fsimg`'s deliberate no-update architecture. |
| Does output require optical tail padding? | Optional, deterministic and validated; logical volume size and host file length accounted separately. | Writers and readers vary in final pad expectations. |
| Is Joliet permitted for every UTF-8 input? | No. Strict UCS-2 character profile; unrepresentable names yield error unless an explicit compatible fallback is selected. | Silent byte truncation or bogus surrogate emission violates the declared format. |
| What is the on-disc default date? | Deterministic explicit epoch/modified timestamp policy; no hidden local timezone dependence. | Reproducibility and metadata fidelity. |
| Can corrupt CE recursion be skipped? | Not in a selected RRIP view; a failure is structural corruption. | Names/types cannot be trusted if continuation is invalid. |
| How many oracles count as independent? | At least different authoring/reading codebases, not merely multiple front ends to one `libisofs` engine. | Prevents false confidence from shared decoder bugs. |

### 22.1 Acceptance checklist for a production claim

- [ ] Every supported namespace is unambiguous, reproducible and independently verified.
- [ ] Writer rejects every unrepresentable required metadata field or entry kind at planning time.
- [ ] All dual-endian numeric fields, checked extent math, and descriptor bounds are fuzzed.
- [ ] Parser never treats arbitrary SUSP bytes as Rock Ridge without handshake validation.
- [ ] Continuation (`CE`) and directory relocation (`CL`/`PL`) are bounded and cycle-safe.
- [ ] PVD and Joliet SVD path tables agree with actual selected hierarchy.
- [ ] Data content is correct across regular, zero-size, interleaved (if supported) and multi-extent boundaries.
- [ ] Shared payloads across ISO/Joliet names and validated hardlinks do not spuriously fail allocation checks.
- [ ] Boot/catalog validation (when supported) is independent of whether firmware can execute supplied bytes.
- [ ] Every accepted writer option is covered by at least one independent consumer and a negative geometry test.
- [ ] Fuzzing, sanitizers, resource quotas and source/sink fault injection have recorded evidence.
- [ ] Each test fixture has provenance, redistribution rights and reproducible generation instructions.
- [ ] The maintained capability records and manual explain all unsupported variants and current limits.
- [ ] `README.md`, `specs/design-formats.md`, `specs/README.md`, `Makefile.am`, `test/` and `fuzz/` are updated only when code for the corresponding stage is implemented.

**End of specification.**
