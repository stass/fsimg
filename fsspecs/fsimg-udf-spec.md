# UDF filesystem format specification and fsimg implementation plan

**Project:** [stass/fsimg](https://github.com/stass/fsimg)  
**Status:** engineering proposal and independent technical specification (not an implementation or certification)  
**Research date:** 2026-10-20  
**Suggested repository location:** `specs/design-udf.md` (after review, with an entry added to `specs/README.md`)  
**Copyright and provenance:** Original explanatory text and algorithms for fsimg. References to ECMA-167 and ECMA TR/112 are links and independently expressed facts, not copied standard text or reference code. Nothing from GPL/LGPL, the Clarified Artistic License, or proprietary source has been incorporated. Code examples herein are original pseudocode, not copied from another implementation. Before incorporating any BSD-licensed implementation material, inspect the **actual per-file license** and retain notices.

> **Specification hierarchy.** The on-disk format is defined by [ECMA-167, 3rd edition](https://ecma-international.org/wp-content/uploads/ECMA-167_3rd_edition_june_1997.pdf) together with the applicable exact UDF revision from [ECMA TR/112](https://ecma-international.org/publications-and-standards/technical-reports/ecma-tr-112/), including its appendices and errata. This document defines **fsimg's supported subset, behavior, architecture, and tests**, and is subordinate to those sources where a discrepancy exists. A numerical layout noted here is a development aid, not a substitute for consulting the authoritative document before emitting bytes.

---

## 0. Executive decisions

UDF is a profile of ECMA-167's volume/file structures, **not** an enhancement record attached to ISO 9660. UDF and ISO 9660 can coexist in a bridge image, but a valid standalone UDF image is independently understandable. Implementing an ISO 9660 reader does not provide UDF semantics, and UDF should not reuse the ISO directory parser. It **can** reuse fsimg's generic bounded I/O, writer catalog, namespace walker, scratch streams, sorting, publisher, and extraction machinery.

**Implementation recommendation:**

1. Reader baseline: detect and validate UDF/ECMA-167 volumes, physical Type-1 partition maps, file set/root, FE/EFE, FIDs, short/long/inline allocation descriptors, symlinks, holes, and UDF 1.02 through 2.60 revisions in *supported layouts*.
2. Reader extensions: VAT (write-once/multisession), sparable partitions, metadata partitions (UDF 2.50/2.60), named streams where representable, and mixed ISO/UDF bridges.
3. Writer baseline: deterministic standalone **UDF 1.02**, sector size 2048 by default, one contiguous physical partition, strategy-4 ICBs, closed logical-volume integrity, space bitmap, VDS redundancy and AVDPs; no media packet writing, modification, sessions, or optical-drive control.
4. Writer expansion: profile for **2.01** (EFE and richer metadata), then **2.50** metadata partition, then **1.50 VAT/sparing** and **2.60 pseudo-overwrite** only with specific interoperability gates. Do not equate an LVD revision string with correct implementation of the corresponding features.
5. Common-core improvement: UDF has **interior sparse holes**, whereas fsimg's existing initialized-length abstraction covers only a zero-filled tail. Extend the *internal* content extent protocol with DATA/ZERO. Preserve the current public reader API.
6. Treat optical drive/packet mechanics, multisession append, encryption/Secure UDF, UDF defect repair and in-place updates as **out of scope for image creation**, unless separately commissioned.

### 0.1 Relevant observed fsimg contracts

The actual repository was inspected, not assumed:

| Existing fsimg subsystem | Observed contract | UDF implication |
|---|---|---|
| `lib/formats/format.h` and `format.c` | One descriptor owns identity, variants, a pure prefix-only probe, and independent reader/writer ops. | Register `fsimg_udf_format`; VRS provides prefix recognition; validate anchors in `open()` after claim. |
| `lib/read/reader.h` | `open`, `close`, `prepare`, `replay`, `keys`, `key`, `resolve`, `verify`. Common walker handles traversal; driver `resolve` returns physical extent. | Directory FID decoder populates `prepare`; partition map resolution stays in driver. |
| `lib/meta/meta.h` and `specs/design-content.md` | `fsimg_extent_t` has `(logical_offset,image_offset,length)` only; one contiguous content domain; `reader->initialized` is a tail boundary. | Requires interior zero runs and a safe domain scheme for UDF extents. |
| `lib/write/writer.h` | Driver `validate`, `plan`, `emit`, `free`, private plan, bounded catalog and frozen input. | Plan all UDF descriptor/data locations before emission; finalize checksums from planned bytes. |
| `include/fsimg.h` | File, directory, symlink, FIFO; owner, mode, change/creation/access/modify times; required-vs-optional metadata loss; resource limits. | Map where representable; mark unrepresentable semantics as losses or unsupported. |
| `lib/read/directory.h` | Prepared children use 64-byte opaque payload, 1020-byte UTF-8/key buffers, driver-provided keys. | UDF locator fits payload; decoded CS0 can be used for keys; reject collisions. |
| `test/spec/` | Ruby 3.2 independent, rule-driven mutation/property tests, replayable seeds, oracle integration. | Add `test/spec/lib/fsimg_spec/udf/` instead of introducing a new test language. |
| README / `CLAUDE.md` | C11, BSD-2-Clause, no non-libc runtime requirements, no mounts/privileges in normal paths, safe extraction, memory and scratch budgets. | External UDF tools are **development/test only**; no runtime link dependency. |

Reference: [fsimg repository](https://github.com/stass/fsimg), [format registration](https://github.com/stass/fsimg/blob/master/lib/formats/format.c), [format interface](https://github.com/stass/fsimg/blob/master/lib/formats/format.h), [reader ops](https://github.com/stass/fsimg/blob/master/lib/read/reader.h), [extent model](https://github.com/stass/fsimg/blob/master/lib/meta/meta.h), [writer ops](https://github.com/stass/fsimg/blob/master/lib/write/writer.h), [API](https://github.com/stass/fsimg/blob/master/include/fsimg.h), [spec harness](https://github.com/stass/fsimg/tree/master/test/spec), and [working agreement](https://github.com/stass/fsimg/blob/master/CLAUDE.md). Access to these repository links requires authorization because it is private.

### 0.2 Terms

* **Sector / LBA**: medium sector and sector number in the image's volume coordinate system. In supported UDF, logical sector size equals physical sector size and logical block size of the logical volume.
* **Volume-space LBA**: absolute block counted from the supplied image/slice origin, not from a filesystem partition.
* **Partition-relative LBN**: position within one physical partition's range.
* **Partition reference number**: **index** into the LVD's partition-map array, not necessarily a physical partition number.
* **ICB**: information control block identifying a file/directory/special object; the ordinary payload is an FE or EFE.
* **FE / EFE**: File Entry, Extended File Entry.
* **FID**: File Identifier Descriptor, a directory record pointing to an ICB.
* **VRS / VDS / AVDP**: Volume Recognition Sequence / Volume Descriptor Sequence / Anchor Volume Descriptor Pointer.
* **PD / LVD / FSD / LVID**: Partition Descriptor / Logical Volume Descriptor / File Set Descriptor / Logical Volume Integrity Descriptor.
* **AD / AED**: Allocation Descriptor / Allocation Extent Descriptor.
* **VAT**: Virtual Allocation Table, used to change logical metadata references on sequential media.
* **Sparing**: packet-level defect relocation table.
* **Metadata partition**: logical partition backed by a metadata file and optional mirror, introduced in UDF 2.50.
* **Bridge**: an image containing UDF and one or more other filesystems such as ISO 9660, sometimes sharing data blocks.

## 1. Normative document collection and revision negotiation

### 1.1 Downloadable standards

| Authority | Revision / scope | URL |
|---|---|---|
| Ecma | ECMA-167, 3rd edition (core structures) | https://ecma-international.org/wp-content/uploads/ECMA-167_3rd_edition_june_1997.pdf |
| Ecma | ECMA TR/112-1, general and revision history | https://ecma-international.org/wp-content/uploads/ECMA_TR-112-1_1st_edition_december_2023.pdf |
| Ecma | ECMA TR/112-2, **UDF 2.60**, incl. errata | https://ecma-international.org/wp-content/uploads/ECMA_TR-112-2_1st_edition_december_2023.pdf |
| Ecma | ECMA TR/112-3, **UDF 2.50**, incl. errata | https://ecma-international.org/wp-content/uploads/ECMA_TR-112-3_1st_edition_december_2023.pdf |
| Ecma | ECMA TR/112-4, **UDF 2.01** | https://ecma-international.org/wp-content/uploads/ECMA_TR-112-4_1st_edition_december_2023.pdf |
| Ecma | ECMA TR/112-5, **UDF 2.00** | https://ecma-international.org/wp-content/uploads/ECMA_TR-112-5_1st_edition_december_2023.pdf |
| Ecma | ECMA TR/112-6, **UDF 1.50** | https://ecma-international.org/wp-content/uploads/ECMA_TR-112-6_1st_edition_december_2023.pdf |
| Ecma | ECMA TR/112-7, **UDF 1.02** | https://ecma-international.org/wp-content/uploads/ECMA_TR-112-7_1st_edition_december_2023.pdf |
| Ecma | ECMA TR/112-8, **Secure UDF 1.00** (recognize, don't decrypt) | https://ecma-international.org/wp-content/uploads/ECMA_TR-112-8_1st_edition_december_2023.pdf |

The **December 2023 date is the Ecma publication date**, not the date a new UDF 2.60 revision was introduced; the underlying revision 2.60 dates to March 2005, and the later republication contains errata. The standards are publicly readable but **not BSD/MIT/permissively licensed program source**. Consult the standards; do not paste their explanatory prose or sample source into fsimg.

### 1.2 Revision feature differences

| Version | Common usage | Feature additions relative to earlier versions | Reader target | Writer target |
|---|---|---|---|---|
| **1.02** (`0x0102`) | DVD-Video, classic DVD-ROM, interoperability | Base UDF subset of ECMA-167, physical partitions, FEs | First | First |
| **1.50** (`0x0150`) | CD-R/CD-RW, packet-writing | Virtual partition + VAT, sparing, UDF bridge profile | After physical baseline | After ordinary writer, optional |
| **2.00** (`0x0200`) | later optical media | Extended file entries, named streams, additional access controls/features | FE/EFE early; streams later | No separate profile required initially |
| **2.01** (`0x0201`) | DVD and general-purpose UDF | Revision refinement; real-time files | Early for ordinary objects | Second |
| **2.50** (`0x0250`) | Blu-ray, large file/archive volumes | Metadata partition, metadata mirror and bitmap | After VAT/sparing or in parallel | Third |
| **2.60** (`0x0260`) | BD-R pseudo-overwrite | Pseudo-overwrite and later optical-media rules | On 2.50 foundation | Last/advanced |
| Secure UDF | encrypted variant | Encryption/authentication layer | Recognize and reject unless implemented | Out of scope |

Feature history summarized independently from ECMA TR/112-1 §5. **Warning:** different revisions permit different legal *combinations*, not merely additional descriptor IDs. A writer that emits UDF 2.50 or 2.60 domain information without satisfying **applicable** metadata-partition and media-profile rules is not conforming.

### 1.3 Revision indicators to inspect

A single `NSR02` or `NSR03` is **not** enough to deduce the complete revision. Combine:

* NSR recognition descriptor; descriptor-version/tag version;
* `*OSTA UDF Compliant` domain entity ID suffix in LVD and FSD;
* `*UDF LV Info` implementation-use data;
* LVID implementation-use fields: minimum read revision, minimum write revision, maximum write revision (when defined);
* VAT-specific revision fields where a VAT is present;
* partition map identifiers, FE/EFE types and feature use.

The opening algorithm is **feature-driven**. It rejects a required feature unknown to the driver with `FSIMG_EUNSUPPORTED`, even if the numeric revision is nominally supported. For a declared 2.60 volume, a reader implementing only ordinary physical partitions may read it if all actually used structures are supported, but must not claim full 2.60 compatibility. A header with inconsistent revisions/features is `FSIMG_ECORRUPT` when demonstrably contradictory; a coherent newer/unknown feature is `FSIMG_EUNSUPPORTED`.

### 1.4 Media profiles

Keep **media intent separate from UDF version**:

* `readonly-image`: closed, non-multisession, no VAT/sparing. Image may be created as a regular file and transferred to DVD-ROM/BD-ROM-style storage.
* `overwritable-image`: ordinary physical partition, possibly a metadata partition at 2.50+; no live updates by fsimg.
* `writeonce-vat`: 1.50+ virtual addressing on sequential media; requires a valid VAT commit and media-model awareness.
* `sparable-packet`: physical packet defect remapping; packet size, sparing tables, defective packet policy.
* `metadata-partition`: 2.50+ Type-2 metadata map with backing file/mirror; subject to media-specific restrictions.
* `pseudo-overwrite`: UDF 2.60 remapped behavior, not equivalent to randomly writing an image.
* `multisession`: separate discovery and latest-session choice, not equivalent to concatenating independent UDF images.
* `bridge`: UDF plus ISO 9660/Joliet/Rock Ridge/HFS+, potentially overlapping in physical data. No guarantee of identical directory trees.

Filesystem *formatting* a byte-addressed image cannot itself establish optical drive track/session state; no writer profile may promise complete device/media conformance solely from raw file bytes.

## 2. Binary fundamentals and address spaces

### 2.1 Endian, primitive sizes, and alignment

UDF/ECMA-167 descriptor numeric fields are **little-endian**. The volume's logical sector and logical block sizes match the physical sector size for the UDF profile. fsimg's proposed writer sizes: `512,1024,2048,4096`; default **2048** for an optical image. Reader may accept additional standards-valid sector sizes, but it must have a reliable means to identify geometry. A raw image provides no device block-size metadata: auto-detect *candidate* sizes from `[512,1024,2048,4096]`, then require a complete valid AVDP/VDS/FSD chain and unambiguous result. Additional sizes are explicit-extension work.

All offsets are checked with overflow-safe arithmetic. When given an `fsimg_io_slice`, byte zero is the **volume** origin; caller's `-b`/`-z` slice offset is not added by the driver.

**Conversions:**

```
absolute_byte(partition_ref, lbn) =
    (physical_partition_start + translated_partition_lbn) * block_bytes

absolute_byte(volume_lba) = volume_lba * block_bytes

extent_byte_length = extent_length_word & 0x3fffffff
extent_type        = extent_length_word >> 30
```

`partition_ref` is the zero-based LVD partition-map **slot**, translated through its map to a PD and ultimately an image byte offset. Do not treat it as the PD's `PartitionNumber`. `TagLocation` is in the coordinate system specified for that descriptor: volume-space for AVDP/VDS, and partition-relative/virtual/metadata-space where the descriptor is addressed. Tests must cover the same underlying sector seen through two different address spaces.

### 2.2 The first 32 KiB and recognition sequence

Bytes `[0,32768)` are reserved from ECMA-167 descriptor allocation. The **Volume Recognition Sequence** begins at a 2048-byte recognition granularity at or after 32768 bytes, with a beginning-of-extended-area descriptor `BEA01`, one `NSR02` or `NSR03` recognizing the intended recording structures, and `TEA01`. ISO 9660 `CD001` descriptors may precede UDF's extended area in a bridge. Media-specific boot descriptors are separate. The identifier bytes occur in the VRS Volume Structure Descriptor header; do not search arbitrary payload for the string.

**Pure fsimg probe:** declare `probe_bytes` enough to include the bounded VRS recognition window (e.g. 64 KiB initially; do not allocate unbounded buffer). Check a plausible ordered VRS at recognized offsets. Because UDF anchors occur later and sector geometry may be ambiguous, a valid VRS is only a **format claim**, not proof of a readable filesystem. `open()` performs bounded positional I/O and validates anchors. A lone `CD001` must never claim UDF.

**Bridge caveat:** the same image may be a valid UDF and ISO 9660 image. fsimg's existing auto-prober refuses multiple claimants. Retain that safety rule and require `-t udf` or `-t iso9660` when both claim, until a deliberate preferred-view selection policy is added. Do not silently favor one hierarchy.

### 2.3 Anchors and volume descriptor sequences

For a conventional complete closed image, inspect candidate AVDPs at **LBA 256**, **last LBA - 256**, and **last LBA**. UDF requires appropriate redundancy (normally at least two of the applicable positions), but media/session constraints can change which candidate is meaningful. Only seek positions within the slice, with block-aligned image size. **Do not accept a solitary checksum-valid anchor without checking the pointed-to sequences.**

An AVDP (descriptor tag ID 2) contains main and reserve VDS extents (byte lengths plus volume-space positions). VDS extents contain, at minimum for the supported standalone profile, a PVD, LVD, PD, applicable implementation-use structures, possible USD, and a terminating descriptor (tag ID 8). `Volume Descriptor Pointer` (ID 3) may extend a sequence; resolve it with cycle limits. Each main and reserve VDS sequence has a minimum allocation of 16 logical sectors in the 2.60 profile. Sequence selection considers descriptor sequences/numbers, validity, and explicit consistency; never choose blindly based on the first valid descriptor.

For read-only image **construction**, place main and reserve VDS blocks apart, reserve their entire declared extents, generate consistent replicated records, and emit redundant AVDPs at valid designated locations. The writer plan must account for small-volume impossibility and end-of-image collisions.

For **reading**, build a coherent volume descriptor set independently from each candidate sequence. Use a consistent valid set, and report any fallback through diagnostics. A full verifier checks disagreement between nominally equivalent main/reserve sets as a consistency failure unless permitted by the revision/media's recovery semantics. A reader may still perform safe recovery from a valid reserve set, but must not report full verification success for unreconciled corruption.

### 2.4 Descriptor tags (16 bytes)

The descriptor tag is the most important reusable primitive. Its **independently restated field layout** is:

| Byte | Size | Field | Requirement |
|---:|---:|---|---|
| 0 | 2 | Tag identifier | Known descriptor ID for context |
| 2 | 2 | Descriptor version | Supported ECMA-167 descriptor version |
| 4 | 1 | Tag checksum | Sum of bytes 0 through 15 excluding byte 4, modulo 256 |
| 5 | 1 | Reserved | Zero for canonical output |
| 6 | 2 | Tag serial number | Volume-related sequence; preserve and cross-check as applicable |
| 8 | 2 | Descriptor CRC16 | CRC over descriptor payload beginning after tag |
| 10 | 2 | Descriptor CRC length | Number of bytes included in payload CRC |
| 12 | 4 | Tag location | Address in this descriptor's coordinate system |

Descriptor CRC: ECMA-167 CRC-16/CCITT-style generator polynomial `0x1021`, initial remainder zero, **no reflected bit order, no final XOR**. Compute on exact specified bytes, never on uninitialized padding. `TagChecksum` and CRC protect different ranges. A bad checksum, CRC, CRC length, location or descriptor version is structural corruption.

**Critical parser rule:** `DescriptorCRCLength` is *not* in general the descriptor's complete size; derive the actual length from the descriptor ID and its bounded variable-length fields. CRC length can be shorter due to the 16-bit field's maximum. The parser independently validates both: (a) bytes needed to parse safely and (b) available CRC-covered bytes.

### 2.5 Important descriptor IDs

| Tag ID | Descriptor |
|---:|---|
| 0 | UDF Sparing Table (UDF extension) |
| 1 | Primary Volume Descriptor (PVD) |
| 2 | Anchor Volume Descriptor Pointer (AVDP) |
| 3 | Volume Descriptor Pointer |
| 4 | Implementation Use Volume Descriptor |
| 5 | Partition Descriptor (PD) |
| 6 | Logical Volume Descriptor (LVD) |
| 7 | Unallocated Space Descriptor (USD) |
| 8 | Terminating Descriptor |
| 9 | Logical Volume Integrity Descriptor (LVID) |
| 256 | File Set Descriptor (FSD) |
| 257 | File Identifier Descriptor (FID) |
| 258 | Allocation Extent Descriptor (AED) |
| 259 | Indirect Entry |
| 260 | Terminal Entry |
| 261 | File Entry (FE) |
| 262 | Extended Attribute Header Descriptor |
| 263 | Unallocated Space Entry |
| 264 | Space Bitmap Descriptor |
| 265 | Partition Integrity Entry (disallowed in OSTA UDF subset) |
| 266 | Extended File Entry (EFE) |

An unknown tag in a mandatory reachable role is unsupported, not a reason to reinterpret those bytes as a known tag. An explicitly skippable implementation-use item can be retained/ignored only after its owning length is validated.

### 2.6 Reusable address and extent structures

| Format field | Wire bytes | Notes |
|---|---:|---|
| `lb_addr` | 6 | `uint32 lbn` then `uint16 partition_ref` |
| `extent_ad` | 8 | `uint32 length_bytes`, `uint32 volume_lba` (VDS/LVID/other *volume-space* extents) |
| `short_ad` | 8 | 30-bit byte length + 2-bit type, then `uint32 partition_relative_lbn` |
| `long_ad` | 16 | 30-bit byte length + type, 6-byte `lb_addr`, then 6 implementation-use bytes |
| `ext_ad` | 20 | ECMA-167 defines it, but UDF profile restricts file allocation descriptor forms; not in baseline supported UDF writer |
| `timestamp` | 12 | local time fields with signed 12-bit timezone; see §7 |
| `regid` / EntityID | 32 | Flags, 23-byte identifier, eight-byte suffix; identifier and suffix semantics depend on role |
| `charspec` | 64 | Character-set type and information; UDF uses CS0 |

The top two bits of a short/long allocation `ExtentLength` word classify the extent:

| Bits | Meaning | Reader delivery |
|---:|---|---|
| `00` | Allocated and recorded | DATA, subject to length and bounds |
| `01` | Allocated but not recorded | ZERO, but physical allocation is owned |
| `10` | Not allocated, not recorded | ZERO, no data allocation ownership |
| `11` | Continuation to another allocation descriptor extent | Follow AED; not file contents |

A zero byte length must carry type `00`. The maximum byte count per AD is `0x3fffffff` (further constrained by block alignment, logical object size, and revision). Logical byte coverage is formed by concatenating non-continuation AD lengths, in order, and must agree with the file's `InformationLength`. Only the terminal extent of the **file body** may be partial-block length when permitted; a continuation describes descriptor storage, not payload. Reject zero-progress continuation cycles.

**Inline allocation:** ICB tag allocation type `3` stores data inside the FE/EFE allocation area. Its byte range is validated against the FE/EFE record and its allocation-length field. `LogicalBlocksRecorded` is zero for fully inline content. Inline directories and tiny files must work; do not assume every file has data extents.

**Fragmented files:** FE/EFE allocation descriptors can reference AED chains (tag 258); AEDs contain further descriptors. Validate tag location relative to each AED's own partition and do not follow an extent twice within the same chain. Charge every hop against `max_work` and disk-space ownership verification.

## 3. Volume structure traversal

### 3.1 Opening algorithm

Suggested driver-private state:

```
udf_state = {
  block_bytes, image_blocks, revision_bounds, source_session,
  selected_anchor, selected_volume_descriptors,
  physical_partitions[], partition_maps[],
  logical_volume_integrity, fileset_address, root_icb,
  vat_state[], sparing_state[], metadata_backing[],
  observed_feature_set, capability_profile, scratch_ownership
}
```

All arrays are bounded by validated on-disk counts and the fsimg session budget. No unbounded linked lists or host-dependent packed struct casts.

Algorithm:

1. Confirm VRS recognition through `probe` or forced format; validate prefix and candidate sector sizes.
2. For each candidate block size and anchor position, read exactly the metadata required for its AVDP and check descriptor tag, CRC and location.
3. Follow main and reserve volume descriptor sequences through bounded ranges and sequence pointers. Reject repeated loops and contradictory mandatory descriptors.
4. Select coherent PVD/LVD/PD set; validate logical block size, partition maps, revision markers, character sets, integrity sequence, and metadata extent bounds.
5. Construct partition translations without trusting content addresses. Resolve physical mapping ranges and any required VAT/sparing/metadata helper structures.
6. Resolve LVD Logical Volume Contents Use (a long AD) to the FSD. Validate file set/domain/Unicode and the root directory ICB reference; optionally read a System Stream Directory ICB for verification, but don't expose unsupported named streams as ordinary files.
7. Resolve root FE/EFE; validate file type and prepare to walk. Populate `fsimg_volume_info_t` from **verified** geometry (label, allocation count, space summaries and status only if grounded).
8. If more than one sector-size/anchor/volume choice remains coherent but semantically different, return ambiguity (`EUNSUPPORTED` or `EFORMAT` per identity vs profile) and suggest explicit geometry/session selection. Never guess.

### 3.2 Partition Descriptors and maps

A PD provides physical partition start and length in blocks, access type, partition number, content ID, and partition header information (space tables/bitmaps). One LVD lists partition maps, each assigned a **map index**. A Type-1 map is six bytes with partition number and volume sequence number and describes straightforward physical mapping.

For a Type-1 map:

```
if 0 <= logical_block < pd.block_count:
    media_lba = pd.start_lba + logical_block
```

For reads of a byte extent, enforce both the physical partition boundary and global `fsimg_io` slice boundary after translation. For a descriptor that lies inside a partition, check its tag location against the original LBN in the **referencing map's** address space, before translating it to a physical sector.

Multiple physical partitions, even on one image, are a separate reader expansion. UDF multi-volume *sets* referencing missing additional discs are unsupported; do not silently merge independent sessions or disc images.

### 3.3 Primary/Logical Volume Descriptor specifics

Validate:

* PVD interchange-level, volume identifier, volume set identifier, descriptor character sets and records;
* LVD logical block size, domain identifier and revision suffix, map-table length and count, `LogicalVolumeContentsUse` (FSD long AD), and integrity sequence extent;
* each PD's `PartitionNumber`, `PartitionStartingLocation`, `PartitionLength`, access type and partition content identifier (`+NSR02`/`+NSR03` as applicable);
* that mapping table descriptors **exactly** tile the declared map-table length and number of entries, with no zero-length, overflow, or nonadvancing parse;
* no unresolved references to a second volume of a volume set in a single-file image.

A VDS terminating descriptor ends traversal within an extent; zeros after it are not permission to read beyond the allocated sequence. If a read-only bridge image has descriptors in an ISO-reserved region, the UDF writer must not overwrite them accidentally; the bridge planner must own shared placement.

### 3.4 Logical Volume Integrity Descriptor (LVID)

LVID (tag 9) is located via the LVD integrity sequence extent. It has an integrity state (open/closed), a timestamp, free-space and size tables per partition map, implementation-use data and typically a logical volume header containing the next unique ID. Interpret maps' free counts according to map type and revision; some table slots use `0xffffffff` to mean unavailable/unknown.

For an ordinary closed image, `IntegrityType = closed` is the writer goal; open/unclean state is **recognized** but full verification should not mark the volume clean. The reader can allow listing if recoverable. Never treat a closed flag as proof of allocation correctness; full `verify` validates ownership itself.

The baseline writer emits **one final closed integrity record** and a matching termination record. The publication step (not the LVID alone) determines durability for fsimg.

### 3.5 Space management descriptors

There are two principal allocation reporting mechanisms:

* Space Bitmap Descriptor (SBD, tag 264): one bit per logical partition block, with **1 = unallocated/free, 0 = allocated**, not vice versa. Validate bitmap bit-count, byte-count, unused trailing bits and tag. Space records may be represented by the PD's partition header descriptor.
* Space table / Unallocated Space Entry (USE, tag 263): block-range allocation descriptors representing unallocated extents; readers must walk and reconcile their reported ranges. In UDF's use, short ADs are the meaningful free-space descriptors; continuation still uses AED safely.

If both exist, full verify compares them where the revision requires. Validate that no block is simultaneously free and assigned to metadata/file data, and that every reserved sequence, space bitmap, FE/EFE, AED and directory extent is accounted for in its correct ownership domain. Special handling is needed for VAT/metadata/sparing; do not apply a single physical bitmap's interpretation to all map types.

### 3.6 Recovery and incomplete volumes

Readers shall distinguish **identity**, **recoverable redundancy**, **readable namespace**, and **fully consistent volume**. A fall back to reserve VDS/metadata mirror may permit `ls` and `cat`; `verify` should report the underlying primary-copy defect. A truncated logical volume, missing FSD, invalid root ICB, impossible partition geometry, or cyclic descriptor graph is never accepted as valid just because a readable file was found.

## 4. Virtual, sparable, and metadata partition extensions

**Design rule:** every UDF address resolution uses a small, explicit, immutable mapping object. The rest of the file/directory parser receives only validated logical/physical extents and never interprets VAT/metadata/sparing arrays by itself. This removes a potentially combinatorial set of special cases from the traversal logic.

### 4.1 Resolver shape and invariants

Proposed driver-internal interface (illustrative, not a public ABI):

```
map_block(partition_ref, lbn, purpose) -> {
    image_lba, domain_id, recorded_state
}
map_run(partition_ref, first_lbn, blocks, purpose) -> {
    physical_contiguous_blocks, domain_id, recorded_state
}
```

`purpose` distinguishes descriptor address, directory bytes, ordinary payload, VAT/metadata auxiliary file, and physical-space accounting. It is **not** a bypass for access checks. On each translation:

1. Look up the partition-map entry by map index; reject missing/unknown map.
2. Validate original LBN against the map's domain and requested range.
3. Apply at most the layer sequence licensed by that map type (Type-1, virtual, sparable, metadata). Do not recurse arbitrarily from attacker-controlled bytes.
4. Clip the returned **run** at each mapping discontinuity, partition end, and physical image boundary. A run is not valid solely because its first sector is valid.
5. Test integer overflow before multiplication and addition.
6. Keep `TagLocation` checks in the *original* logical/virtual/metadata coordinate system; do not replace with translated physical LBA.

A sector may validly be referenced by multiple **logical** addresses under metadata or shared structures. Ownership checking therefore needs two concepts: ownership of backing physical storage, and intentional logical aliases. Cross-linked *unrelated regular files* must still fail.

### 4.2 Virtual partition and VAT (UDF 1.50+)

**Purpose:** make an append-only physical volume appear writable for metadata by changing a table rather than overwriting previous metadata.

Identification: a Type-2 64-byte partition map, entity ID `*UDF Virtual Partition`, partition number referring to the underlying physical partition. A virtual map coexists with the underlying physical Type-1 map. The Type-2 map has its own map index in the LVD's array. **The VAT is not a disk-wide physical relocation map for all file bytes**: it primarily makes metadata/ICB virtual references stable across append transactions.

Canonical 2.00+ VAT file facts:

* A dedicated ICB with file type **248** identifies VAT content.
* On sequentially recorded media the VAT ICB is the **last recorded valid sector of a transaction**; earlier VAT records may exist and form a history.
* The VAT file may use embedded data or file extents.
* Its header includes its own header length and implementation-use length, logical-volume identifier, previous VAT ICB location, counts, and UDF revision information. The table follows the header as 32-bit little-endian block numbers.
* Entry `i` resolves **virtual LBN `i`** to a partition-relative physical logical sector; unmapped entries use the revision's sentinel (`0xffffffff` in the ordinary mapping).
* The previous-VAT pointer supports historical recovery but does not authorize selecting stale metadata silently.

**UDF 1.50 differences:** its original VAT format uses a **trailing information block/footer** rather than the later header layout. Implement separate version-specific codecs selected using observed revision and length constraints; never use the newer 152-byte header decoder on a 1.50 VAT. Consult ECMA TR/112-6 VAT sections and NetBSD's `udf_subr.c`/`newfs_udf` implementations for precise predecessor fields and identification policy before implementing this case.

**Discovery without mount:** the UDF specification's commit semantics depend on the *last recorded sector* and session state. A regular byte file gives a known length, **not** proof that its last bytes were all recorded by an optical drive. For first-phase VAT support, require a closed flat image whose final valid VAT ICB can be found in an explicitly bounded tail search, with coherent tag/ICB/volume identity. Record provenance and any fallback in `info`. A sparse/truncated/append-in-progress image must not induce an open-ended backwards scan; charge a fixed or user-limited search window and return `EUNSUPPORTED` if no authoritative latest VAT is determinable.

**Reader rules:**

1. Validate the VAT ICB and type 248 *through the underlying physical map*.
2. Parse FE/EFE allocation safely without depending on the virtual map being built (bootstrap dependency must not be cyclic).
3. Validate table length and entries; keep on disk/scratch rather than requiring a full VAT in RAM.
4. `virtual_lbn -> vat[virtual_lbn] -> physical_partition_lbn -> image_lba`.
5. Validate virtual ICB tag location against virtual LBN even if underlying physical sector differs.
6. For files/directories under virtual ICBs, **file contents normally reference the physical partition through `long_ad`**. Short ADs in virtual ICBs would implicitly point back into virtual space and violate the required UDF allocation policy; reject unsupported/invalid combinations.
7. Detect previous-VAT cycles, duplicates, nonsensical physical addresses and VAT/data overlaps that cannot be validly explained.

**Writer:** defer VAT emission until a tested sequential-media profile exists. If added for a *single immutable image*, emit payload first, physical metadata/data, VAT body, and **last** VAT ICB, with an appropriate tail, anchoring/session model, and independent kernel/udftools checks. This is *not* an in-place filesystem update API.

### 4.3 Sparable partition (UDF 1.50+)

Identification: Type-2 partition map entity `*UDF Sparable Partition`, replacing the normal Type-1 map for the mapped partition. Standard 64-byte map field summary:

| Relative byte | Size | Meaning |
|---:|---:|---|
| 0 | 1 | Type = 2 |
| 1 | 1 | Map length = 64 |
| 4 | 32 | Entity identifier `*UDF Sparable Partition` |
| 36 | 2 | Volume sequence |
| 38 | 2 | Physical partition number |
| 40 | 2 | Packet length in sectors |
| 42 | 1 | Number of sparing tables (1–4) |
| 44 | 4 | Reserved byte length for *each* table |
| 48 | 4 × count | Table **media LBAs** |

Each table (tag 0) contains packet relocation pairs: an **original partition-relative packet address** and a **replacement absolute volume-space packet LBA** (not another partition-relative address). Mapping is at **packet** granularity: align a requested partition-relative address to its original packet, apply the absolute replacement packet, and restore the sector offset *within* that packet; do not add the physical PD start twice. The medium's defect-management behavior controls when a sparable partition is appropriate; do not enable it on a device already doing transparent hardware sparing just to advertise “full UDF support.”

Validation includes:

* packet length positive, maps/tables fully inside image, packet alignment and table-capacity bounds;
* table identifier, checksum/CRC, sequence/copy consistency and entry count;
* original packet addresses unique and non-overlapping, mapped spare destinations unique, in range and packet-aligned;
* mappings not circular and no remapped packet also used for unrelated ordinary data;
* entries with reserved unused/defect markers handled according to the selected revision;
* auxiliary table locations and spare packets accounted for as special allocated regions;
* divergent copies reported as a verify defect; a recoverable consistent copy can be used for read.

Prefer two physically distant sparing-table copies in a writer. Do **not** implement writes to failing sectors: fsimg plans complete files, not a hardware defect-remap loop. For a deterministic image-generation profile, allow a predeclared sparing map only after there are real interoperability tests and a controlled design for reserving spare packets.

### 4.4 Metadata partition (UDF 2.50+)

**Reason for existence:** cluster filesystem metadata into a separate logical address space so optical playback and metadata access require fewer seeks; optional duplication protects metadata. The on-disk metadata partition is not an ordinary disjoint span of sectors. It is a **logical file-backed partition**.

Type-2 Metadata Partition Map, exactly 64 bytes:

| Byte | Size | Meaning |
|---:|---:|---|
| 0 | 1 | Map type = 2 |
| 1 | 1 | Map length = 64 |
| 4 | 32 | `*UDF Metadata Partition` EntityID |
| 36 | 2 | Volume sequence |
| 38 | 2 | Underlying partition number |
| 40 | 4 | Metadata File FE/EFE ICB **partition LBN** |
| 44 | 4 | Metadata Mirror File FE/EFE ICB LBN |
| 48 | 4 | Metadata Bitmap File FE/EFE ICB LBN (sentinel if absent) |
| 52 | 4 | Allocation unit, in logical blocks |
| 56 | 2 | Alignment unit, in logical blocks |
| 58 | 1 | Flags; bit 0 requests duplicate metadata backing |
| 59 | 5 | Reserved zeros |

The backing objects live in the **physical/sparable** partition, with ICB file types:

* 250 = main Metadata File;
* 251 = Metadata Mirror File;
* 252 = Metadata Bitmap File (where permitted/required).

For an ordinary UDF 2.50/2.60 medium with a single PD of access type 0 (pseudo-overwritable), 1 (read-only), or 4 (overwritable) **and without a virtual map**, the metadata partition is required under the cited profile conditions. A 2.50 writer that places the entire tree in a Type-1 physical partition while skipping the metadata map can be **nonconforming** even if a tolerant reader mounts it. Explicit read/write profile constraints prevent such false claims.

**Address translation:**

```
metadata logical lbn
 -> block/extent of Metadata File at that logical file position
 -> physical or sparable partition lbn
 -> absolute image LBA
```

If the duplicate metadata flag is set, the mirror file uses **separate** allocated blocks; if not, its allocation can refer to the same physical blocks under the revision's rules. Check both FE/EFE structures and mapping consistency. Fail over to the mirror only after the primary's validation fails and mirror's relevant region is independently valid; remember degraded status for `verify`.

Important location rules:

* FSD, ordinary FE/EFE, AED and directory/stream-directory content are typically in the **metadata** logical partition.
* Regular file data and ordinary named-stream payloads are in the **physical** partition, and their allocation descriptors select the appropriate map.
* Metadata-space descriptors use short ADs for other metadata-space extents; physical file-data extents use long ADs selecting the physical map.
* A type-3 AD continuation points to the same partition as the descriptor that contains it.
* Metadata-space `TagLocation` identifies metadata LBN, not the backing physical file's sector.
* A metadata bitmap describes **metadata logical blocks**, while the underlying physical partition's allocation bitmap accounts for the backing file and normal payload extents. Do not compare these bitmaps as if they share one coordinate system.
* A metadata bitmap file is **not** written for read-only or pseudo-overwritable partitions where the specification forbids it.
* Allocation-unit size and alignment must obey the media ECC/packet constraints. The 2.60 rules require allocation unit size based on at least 32 sectors/ECC/packet constraints; do not blindly fix it to 32 for every medium.

**Bootstrap:** the physical mapping and Metadata File FE must be decoded **without** using the metadata map being built. Parse the backing FE as a normal file in the underlying physical partition and use its validated, non-cyclic allocation descriptors to build the virtual metadata address space. Mirror and bitmap follow similarly. Then and only then resolve the FSD/root through metadata mapping. This bootstrap ordering makes the reader's dependency graph acyclic by construction.

**Writer strategy:** generate an ordinary 2.50 profile from fsimg's immutable catalog:

1. Plan **all** metadata records as a separate logical metadata byte stream: FSD, directory FE/EFE, directory FIDs, AEDs, and any eligible metadata special files.
2. Plan ordinary file payload extents in physical partition space.
3. Allocate physical extents for the Metadata File and, if the selected profile requires, a **separate** mirror; reserve allocations before computing the physical space bitmap.
4. Plan physical FE/EFE for metadata main/mirror and, when legal, metadata bitmap.
5. Write metadata stream once; duplicate the exact validated logical contents to mirror (where truly mirrored, while preserving distinct physical addresses); write individual per-descriptor tag locations as **metadata** logical addresses.
6. Emit physical-file payload and space accounting, LVD map entries, PDs, main/reserve VDS, LVID, FSD and anchors in the agreed complete order.
7. Independently validate metadata partition + mirror using NetBSD `fsck_udf` and mount/remount tests, not just `udfinfo` header parsing.

### 4.5 Pseudo-overwrite UDF 2.60 and Blu-ray extensions

Pseudo-overwrite (POW) is a property of a **sequential write-once medium and its drive's logical overwrite abstraction**. It is not equivalent to simply replacing an image's bytes and changing the version marker to 2.60. Reader must recognize correct PD access type and metadata map/other profile constraints. Writer for ordinary raw files should initially **reject `media=pseudo-overwrite`** until the behavior can be exercised with a genuine simulator or real BD-R media oracle.

Some UDF appendices define media-specific rules for DVD/BD, ECC block alignment, realtime data, logical overwrite, VRS/anchor placement and multisession. Keep optical-specific geometry and session conditions as a **profile module**, not scattered exceptions to one “universal UDF” writer. BD-ROM/read-only images and general-purpose UDF2.50 images are also not interchangeable as conformance claims.

### 4.6 VAT / metadata / sparing combinations

Do not infer that arbitrary combinations of Type-2 maps are legal. Validate the profile's allowed combinations, underlying map references and ordering. Examples to **test and distinguish**:

* Physical Type-1 only, ordinary UDF 1.02/2.01.
* Physical Type-1 + virtual VAT map (UDF 1.50+ sequential).
* Single sparable map replacing the corresponding physical Type-1 (packet media).
* Type-1 physical + Type-2 metadata map (UDF 2.50/2.60 read-only/overwritable).
* Sparable + metadata map where that medium/revision allows metadata to be backed by sparable storage.
* Any unknown Type-2 identifier, unknown suffix revision, duplicate map entry, cyclic backing, or illicit profile combination.

The source of truth for the last case is the **exact revision's** ECMA TR/112, not a permissive behavior observed in one OS.

## 5. File structure, allocation, and directory traversal

### 5.1 File Set Descriptor and root

FSD (tag 256) is addressed from the LVD `LogicalVolumeContentsUse` long AD, after partition resolution. It records the file-set identity, root directory ICB long AD, file set charset/domain, optional System Stream Directory ICB and associated metadata.

The root ICB resolves to an FE or EFE with file type directory (ICB type 4). Root unique ID is ordinarily zero. Validate the file set's revision/domain consistency with the LVD; do not accept a file set from the wrong logical volume just because the tag CRC is correct.

A file set's identifier/metadata and the volume's label are distinct fields. A label shown by `fsimg info` should be the **logical volume identifier** (after safe CS0 decoding) with any mismatch against related PVD/FSD fields visible in extended diagnostics.

### 5.2 FE/EFE, ICB tag and file types

ICB Tag contains strategy type, file type, parent ICB location in relevant contexts, flags describing allocation-descriptor form, and other attributes. Accepted baseline strategies: **4** (ordinary direct), **4096** where the revision and media allow an indirect chain (handle safely, not required for writer). Unknown or recursive strategy chains are `EUNSUPPORTED` or `ECORRUPT` according to evidence.

Relevant ICB file types:

| Type | Meaning | Initial fsimg treatment |
|---:|---|---|
| 4 | Directory | Read and traverse |
| 5 | Regular file | Read, cat, extract, write |
| 6 | Block special | Verify identity; do not extract as device |
| 7 | Character special | Same |
| 8 | Extended attributes | Parse conservatively, not expose as file |
| 9 | FIFO | Expose kind, safe extractor may skip |
| 10 | Socket | Unsupported for public entry model |
| 11 | Terminal | Unsupported |
| 12 | Symbolic link | Decode component stream; expose kind |
| 13 | Stream directory | Verify, do not expose as ordinary directory without API |
| 248 | VAT | Internal only |
| 249 | Realtime file | Recognize but do not access ordinary file data without the required media semantics; report `EUNSUPPORTED` |
| 250/251/252 | Metadata main/mirror/bitmap | Internal only |

The common API does not support device nodes, sockets, hard-link identity, or named streams; **do not convert them to misleading regular files silently**. For known nonrepresentable kinds, either return `EUNSUPPORTED` (default) or make a deliberate safe listing/extraction-skip policy through the existing `-k` mechanism when appropriate.

Relevant FE/EFE data to parse:

* owner UID/GID, UDF permission mask, hard-link count;
* record format/display/length (UDF expects ordinary unspecified stream style, no hidden record parsing);
* `InformationLength` (logical file size, 64-bit), EFE `ObjectSize` where applicable, and `LogicalBlocksRecorded`;
* access, modification and *attribute* times; EFE also creation time;
* extended attributes ICB/inline EA region, length of extended attributes and length of allocation descriptors;
* implementation ID/entity suffix, unique ID;
* directory stream directory ICB in EFE where supported.

Verify `sizeof(fixed_header) + EA_length + AD_length` fits the *actual* FE/EFE storage record and its permitted block. Do **not** use compiler packing or C struct overlays on hostile input. Decode offsets explicitly with bounded primitives.

### 5.3 File Identifier Descriptors (FID)

A directory's data stream is a sequence of FIDs (tag 257), not ISO 9660 directory records. A FID contains a file version, characteristics flags, an ICB `long_ad`, an implementation-use byte length, and the CS0-encoded file name. Its serialized length is `align4(fixed_header + imp_use_len + filename_len)`; verify the padding and all bounded arithmetic.

Important flags include deleted, directory, and parent. A parent FID has an **empty file identifier**. Its Directory flag depends on the referenced object, including the special case of a named-stream directory pointing to its parent regular file. Ordinary entries must have a nonempty supported name; no directory hard links.

For each non-deleted FID:

1. Validate tag checksum/CRC/location and full variable record length.
2. Decode file identifier through CS0, rejecting unrepresentable codepoints, illegal name/path separators for the common model, and embedded NUL.
3. Reject duplicate **decoded** names within the directory (two different byte encodings producing the same Unicode name are duplicates). Apply any version-specific normalized-equivalence rule where explicitly required; avoid arbitrary host case folding.
4. Resolve ICB `long_ad` map and byte length; validate FE/EFE kind matches FID directory flag.
5. Check UniqueID assignment and matching requirements, including FID's `long_ad` implementation-use `UniqueID` and target FE/EFE `UniqueID` where prescribed.
6. Admit the child to `fsimg_directory_admit` with an opaque locator containing only validated object identity `(partition_ref,lbn,icb_type,revision...)`; keep expensive extent information out of the 64-byte payload.
7. Charge each FID and depth work. Reject ancestor cycles and invalid directory-referenced graph paths. Record hard-linked FE references for later link-count verification, without falsely classifying legitimate multiple file names as cycles.

Deleted FIDs must not appear in normal listing. Later revisions can alter the compression ID of a deleted name (e.g. 254/255 variants); do not treat those as live file names.

**Directory boundary nuance:** individual FID records are bounded by a logical block for the UDF profile, but a record's tag/padding and block-placement rules are media-dependent. A reader consumes the logical **directory byte stream** and supports a record crossing a physical extent or block boundary where legal; it must not read beyond `InformationLength`. A writer can deliberately pad/place FIDs so tags do not straddle blocks and make its own generated profile simpler. Never derive FID size solely from the CRC length.

### 5.4 Directory storage and large directories

UDF directories can be inline inside FE/EFE or described by short/long ADs and AED continuations. They can span many noncontiguous extents. Their byte stream contains FIDs and maybe padding; the ICB's `InformationLength` bounds the stream.

fsimg has a good fit: `prepare` decodes exactly one directory, streams names into scratch, constructs the stable directory key index, then releases transient FID buffers; `replay` rechecks stored locator/name and validates the FE/EFE before exposing the entry; `resolve` maps regular-file content runs. Do not build the whole directory tree in memory. Sorting can be external. Cap per-directory bytes/entry count/path depth with the existing session budgets.

Namespace semantics:

* A UDF file identifier is Unicode, not a POSIX byte filename. Preserve **displayed UTF-8** and collision checks independently.
* UDF ordinarily permits case-preserving, case-sensitive names; fsimg should **not** impose FAT's case-insensitive behavior.
* For filenames that are legal in UDF but unavailable in fsimg's cross-platform path component model (e.g. a `/` separator, embedded NUL, too-long host component), reject or encode with an **explicit reversible escape policy**; no silent replacement/collision.
* A filename with a filesystem-specific version suffix (ECMA file versions) requires explicit policy. UDF generally constrains versioning to a single current file; reject unsupported alternative-version semantics.

### 5.5 Data extents and interior holes

Example original test model for a 16 KiB regular file, LBS=2048:

| File offset | Length | UDF AD type | Result |
|---:|---:|---|---|
| 0 | 4096 | `00` | Read sectors A, A+1 |
| 4096 | 2048 | `10` | Return 2048 zeros, no allocated blocks |
| 6144 | 2048 | `01` | Return zeros; allocated backing must be owned |
| 8192 | 8192 | `00` | Read sectors B..B+3 |

A simple `reader->initialized = file.size` is **insufficient**: the interior zero spans must never return whatever arbitrary old bytes happen to exist at physical locations.

**Common-core change (small, scoped):** extend the *private* resolved extent with a kind, e.g. `FSIMG_EXTENT_DATA` / `FSIMG_EXTENT_ZERO`, preserving existing DATA semantics for FAT/exFAT/QNX6. For `ZERO`, `image_offset` is meaningless, and common delivery fills caller buffers directly, subject to the same bytes-work, cancellation, and progress accounting. Extend optional scratch extent indexes to store kind and checksum/check invariants; merge adjacent zero runs. Public `fsimg_range_kind_t` already supports `FSIMG_RANGE_ZERO` for sources; reuse conceptual names, not necessarily ABI.

The present `fsimg_reader_next_range` behavior reports initialized length/tail zeros, not every interior hole; decide whether to expose actual interior ZERO ranges. It is useful and semantically defensible to do so, **but updating public documentation and conformance tests is required**, because users might rely on current shape. It is also possible to preserve coarse DATA range reporting while guaranteeing content reads synthesize zeros; record the chosen policy explicitly.

**Domain improvement:** the current `fsimg_extent_domain_t` accepts one contiguous byte range. A UDF physical partition can have many discontinuous mappings, but each returned physical **run** can still be checked against the whole image. Merely widening `content_domain` to the full image would permit a driver bug to point regular content into descriptors. Prefer an optional *driver-supplied immutable interval set* checked by the common delivery/index on every DATA run, built from validated payload-eligible partition extents. Alternatively use a driver-private resolver that returns a proof/validated domain identifier and a physical run then is checked by a common domain table. Avoid allocating one domain interval per file fragment; domain count should relate to physical partition-backed ranges, not number of files. Metadata/regular payload may inhabit distinct ownership domains; preserve that distinction for `verify`.

**Huge and fragmented files:** do not allocate one huge AD table. Traverse descriptors through bounded scratch, cache current segment for sequential reads, use on-demand indexed lookup for random `cat`/reader seek. The `fsimg_extent_index` can be extended to include zero runs and partition/provenance tags; every scratch record must be validated when read back, not assumed trustworthy.

### 5.6 Ownership, link counts and block slack

Each allocated extent is classified as one of: reserved volume structures, metadata/special file backing, file/directory data, AED data, VAT backing, sparing backing, or free space. Full verify builds a scratch-backed sort or bitmap keyed by **physical sector or allocation unit** and checks clashes. Legitimate physical aliasing must be identified *before* rejecting overlaps: metadata mirror without duplication, hard links to one FE, and multiple logical references through a VAT to a descriptor are not necessarily double allocation.

For each reachable file, verify:

* each allocated sector is in a claimed allowed partition/domain;
* descriptor ownership cannot enter VRS/AVDP/VDS/bitmap/unallocated ranges;
* file logical bytes tile exactly `[0,InformationLength)` without overrun/overlap;
* only legal final extents are partial-sector and slack is never readable through `cat`;
* `LogicalBlocksRecorded`, link counts, UID fields and free-space bitmap/tables are reconcilable;
* hard links point to the same ICB without pretending to be independently owned payloads; directory multiple-parent cycles remain illegal.

The reader can list an image before doing whole-volume ownership verification; the **full `verify` result must be distinct from `ls`/`info` success**.

## 6. On-disk names, identifiers, strings and symbolic links

### 6.1 OSTA CS0 encoding

UDF identifies the **OSTA Compressed Unicode** character set, conventionally as `CS0`. Variable names use a leading compression identifier followed by code units:

* **8**: each following byte holds one code point in the permitted 8-bit range.
* **16**: following pairs store 16-bit code units in **big-endian** byte order (even though numerical descriptor fields are little-endian).
* **254/255**: deleted FID variants in revisions that use the deleted-name convention; these are not ordinary live encodings.

A valid 16-bit form has an even payload count, and an encoded filename cannot exceed the standard's **255-byte file-identifier field**. Validate codepoint ranges and Unicode surrogate handling against the exact revision. Store UTF-8 in the common API after decoding; count **encoded UDF bytes**, not host UTF-8 bytes, when enforcing UDF limits.

**Writer encoding policy:** prefer 8-bit compression when all code points are representable and otherwise 16-bit compression; choose an explicit Unicode normalization policy grounded in the selected UDF revision. Newer 2.60 errata discuss Unicode versions and normalization; do not quietly normalize names in one direction on read and another on write. Validate collisions on the exact decoded-name interpretation that `keys()` and `key()` will use.

### 6.2 Dstrings versus FID strings

A `dstring` is a **fixed-size field** such as volume identifier. The final byte records length of the encoded string (which includes the CS0 compression identifier for nonempty strings); unused bytes between the end of the encoded name and trailing length byte are zero. An empty field is all zero. Verify final length is within field capacity and encoded data is a valid CS0 name.

A FID filename, by contrast, carries a **separate length** in the FID itself. Do not apply dstring trailing-length rules to it. Volume identifiers, file-set identifiers and file identifiers also have distinct maximum lengths; the presence of the same CS0 encoding does not make those lengths interchangeable.

### 6.3 EntityID / regid

A 32-byte EntityID contains one flags byte, a 23-byte identifier field and an 8-byte suffix. The suffix may hold a revision and format- or developer-specific information; **suffix semantics depend on which EntityID it is**. Validate `*OSTA UDF Compliant`, `*UDF LV Info`, `+NSR03`, `*UDF Virtual Partition`, `*UDF Sparable Partition`, `*UDF Metadata Partition`, implementation identifiers and any known media-specific IDs in their actual permitted slots. Reserved bytes must be zero for canonical writer output; readers should reject violations when they change semantics or are expressly forbidden and otherwise report strict-verification differences.

Do not copy a purported operating-system ID blob from a random specimen. Generate a documented fsimg implementation identifier where permitted and use revision-correct suffix rules.

### 6.4 Symbolic links

UDF symlink file contents are a sequence of **path components**, each with a component kind, component byte length, component-file version, and optional CS0 component identifier. Component kinds distinguish root, current directory, parent, and ordinary name; there may be special volume/mount-root semantics. It is *not* a raw NUL-terminated POSIX path.

Reader algorithm:

1. Walk component headers to the exact link content size, checking nonzero progress and every component length; reject unknown/ambiguous root/mount combinations.
2. Decode ordinary name components with CS0 and version constraints.
3. Reconstruct a slash-separated UTF-8 target with exact relative/absolute intent; never resolve it or follow it during extraction.
4. Check resulting bytes against fsimg's **1–1023 byte** symlink target contract. Longer/invalid targets are `EUNSUPPORTED` for representation (not silently clipped).
5. Carry mode/uid/timestamps separately from target data and avoid treating symlink target bytes as a file payload.

Writer performs the inverse component encoding from the existing source's symlink target. `/`, `.`, `..`, absolute root, UTF-8 and percent/escape lookalikes are separate test cases; reject host syntax that cannot be represented safely. UDF may encode link components that are not POSIX-path-equivalent; no lossy conversion without explicit policy.

### 6.5 Hard links, streams and extended attributes

* **Hard links**: UDF permits multiple FIDs referencing one ordinary file FE/EFE with a matching link count; fsimg's entry API has no explicit file-object identity, so read can expose both paths (same bytes) while full verify counts one payload owner. Writing hard links as distinct files is semantically lossy; a future API could add an optional object-ID/link-group field. Until then, don't claim hard-link preservation.
* **Named streams**: EFE stream directory ICB (type 13) and alternate stream FIDs are separate from ordinary child directories. A normal `ls` should not inadvertently surface system streams. Read and verify known metadata stream structures; if user-facing streams are requested, require an API extension, not silent transformation of streams to files.
* **Extended attributes**: FE/EFE can carry embedded or externally addressed EA areas, beginning with an Extended Attribute Header Descriptor and typed EA records. Parse bounds/type/lengths; known creation-time/device/implementation EAs can be consumed where relevant. Unknown arbitrary EA contents are never executable and never trusted as filesystem offsets.
* **ACLs and UDF permission extensions**: only map expressible owner/mode bits. Non-POSIX delete/attribute permissions and ACLs exceed fsimg's current metadata model. Decide loss policy per field and whether a future xattr/ACL representation is needed.

## 7. Metadata and preservation policy

### 7.1 Timestamp decoding and encoding

ECMA timestamp is twelve bytes:

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 2 | Upper four bits: timestamp type. Lower twelve: signed timezone offset in minutes; special unknown value `-2047`. |
| 2 | 2 | Signed Gregorian year |
| 4–9 | 6 | Month, day, hour, minute, second, centiseconds |
| 10 | 1 | Hundreds of microseconds |
| 11 | 1 | Additional microseconds |

The fraction is `centiseconds × 10,000 + hundreds_of_microseconds × 100 + microseconds` (measured in microseconds) and therefore offers up to **microsecond resolution**, not nanoseconds. Ensure field values compose a fractional second less than one million microseconds. Validate dates (including leap years), offsets and the special unknown-zone encoding. A malformed time field is not grounds for fabricating an epoch value.

Mapping into `fsimg_timestamp_t`:

* Known zone offset: preserve `FSIMG_TIME_OFFSET`, `offset_minutes` and normalized UTC Unix seconds. The timestamp's calendar fields represent **local** time at that offset.
* Unknown offset: preserve wall-clock semantics as `FSIMG_TIME_WALL`, not `FSIMG_TIME_UTC`.
* When definitively UTC, use `FSIMG_TIME_UTC` if compatible with the wire representation.
* Set `precision_ns` to `1000` when the full fractional range is meaningful; do **not** claim nanosecond precision. Writer rounds/truncates source nanoseconds according to required/optional metadata policy and reports loss if needed.
* Reproducible writer uses fsimg's configured epoch and deterministic offset rule, not host local timezone.

`fsimg` distinguishes **created**, **modified**, **accessed** and **changed** (POSIX inode change). UDF distinguishes *access*, *modification*, *attribute* and often *creation*. UDF **AttributeTime is not necessarily semantically identical to POSIX ctime**. The writer must explicitly decide whether to map changed -> AttributeTime as a documented approximation, and report representational loss when the field is required and the semantics cannot be preserved. A conservative implementation declines exact `FSIMG_META_CHANGED` fidelity on versions/profiles where semantics differ.

### 7.2 Permission bits, UID/GID, and attributes

UDF `Permissions` is **not simply POSIX `mode & 07777` serialized into a 32-bit integer**. It encodes separate owner/group/other access, execute/search, attribute-change and delete permissions. Implement a named, table-driven conversion from ECMA/UDF permission masks to the subset of POSIX read/write/execute modes. Handle directory search separately from regular file execute. UDF ICB flags also represent setuid/setgid/sticky-like state and must be converted with the correct individual bit definitions; do not assume bitwise identity.

UID/GID use 32-bit fields and have an **unspecified** sentinel (`0xffffffff`). Treat that as unknown/absent, not `uid=4294967295` that would later be chowned. fsimg's extractor intentionally refuses to restore owners, modes or change time in safe extraction, so reading metadata for `info`/`ls` does not grant host privileges.

`FSIMG_META_ATTRIBUTES` includes read-only, hidden, system, archive. UDF may encode comparable attributes in FE/EFE and ICB/file characteristic bits or platform extensions, but they are **not fully portable 1:1**. Advertise only fields verified to round-trip in selected writer profile. If a caller marks a metadata field `required`, format planning must reject unrepresentable values; if optional, record a loss and respect `FSIMG_WRITE_ALLOW_LOSS` semantics. Do not over-advertise format-wide metadata capabilities based on isolated conventions.

### 7.3 Root, uniqueness and deterministic IDs

UDF's root FE/EFE UniqueID is zero; new user objects draw monotonically from logical-volume `NextUniqueID` with reserved lower values. In 2.60, the logical-volume header describes a starting value of **16**, reserving 1–15 for historical Macintosh uses. For each object's first FID, the low 32 bits of FE/EFE UniqueID and FID's implementation-use UniqueID agree; subsequent hard-link FIDs consume their own distinct link ID. Revision details belong in the encoder profile.

For reproducibility:

* Allocate IDs in fsimg canonical catalog order; do not use host inode values.
* Use an explicit volume-set identifier/identity policy; **do not pretend UDF has a universal native UUID field** equal to fsimg's `volume_uuid` unless a documented compatible mapping is implemented. Various tools derive a pseudo-UUID from the volume set identifier, but that is a convention, not an RFC 4122 on-disk field.
* Preserve creation/access/modify times when possible, honor required metadata and report losses before the first sink write.
* Use fixed implementation identifiers/version strings appropriate to the selected profile, not current OS/build path by default.

### 7.4 Suggested capability reporting

A writer's `fsimg_format_info_t.metadata_fields` describes fields the **selected profile** can represent, not fields observed once. Proposed claims:

| UDF profile | File kinds | C/A/M times | UID/GID/mode | Symlink/FIFO | Changed time |
|---|---|---|---|---|---|
| `udf102` | file, dir, symlink, FIFO (where validated) | A/M, creation via File Times EA | Subject to validated UDF permission mapping | Yes with tested codec | Loss/conditional |
| `udf201` | as above | A/M, creation via EFE | As above | Yes | Loss/conditional |
| `udf250` | as above + file-backed metadata partition | EFE | As above | Yes | Loss/conditional |
| `udf260` | as above when fully implemented | EFE | As above | Yes | Loss/conditional |

For accurate reporting, consider a *variant-specific* `metadata_fields` mask and a writer-plan representability check that can return required field failures. The UDF profiles should not advertise `FSIMG_META_CREATED` until the writer actually emits and round-trips the relevant EFE/EA.

## 8. Writer: complete-image layout, geometry and planning

### 8.1 Writer goals

The first UDF writer should be **small, inspectable, deterministic, and interoperable**. It writes one complete closed image; it never implements mutation, transaction commits, journal replay, optical drive packet-writing, remounting or append-in-place. It must produce all required structures including allocation records; a file with merely a valid VRS and root FE is **not** success.

The writer must meet fsimg's existing operation sequence:

```
validate(options, variant)
    -> add files/catalog, resolve source identity
    -> plan(frozen catalog, limits)
    -> emit(initialized sink)
    -> publish through common coordinator
```

The plan has explicit ownership of (1) volume-space layout, (2) physical partition allocation, (3) metadata/ICB addressing, (4) FIDs, (5) free-space accounting, and (6) descriptor checksum bytes. No plan stage rereads source file data just to discover object sizes.

### 8.2 Writer profile names, size and CLI

Initial public format name and possible future writer profiles:

* `udf` — the single format name and forced-reader selector. On initial completion its writer emits **UDF 1.02 only**. A reader can accept qualifying later revisions without changing the selected name.
* `udf102` — possible future explicitly named writer profile (an alias only if the variant/capability contract cleanly supports one).
* `udf201` — later FE/EFE writer profile; creation time without File Times EA where EFE is used.
* `udf250` — later metadata-partition writer profile.
* `udf260` — deferred advanced/media-specific writer profile.

Because one fsimg descriptor shares reader/writer operations across its variants and the existing registry derives capabilities from those shared operations, start with **one `udf` format**, whose reader accepts qualified UDF revisions and whose initial writer emits **only UDF 1.02**. Separate public writer profiles (`udf102`, `udf201`) require corresponding per-variant capability handling or a writer-profile selection mechanism. Do not advertise `udf102` as the only writer-capable variant while `udf` remains read-only under the current shared-ops implementation; see §15.

Current `fsimg_write_options_t` already has `sector_bytes`, `allocation_unit_bytes`, `volume_bytes`, `min_free_bytes`, `epoch`, `label`, `volume_uuid`, `reserve_percent`, and `FSIMG_WRITE_REPRODUCIBLE`. Use these; don't introduce a separate settings interpreter if a narrower UDF-specific option can be handled by a format variant. Under current header, sector size defaults to **512 globally**. The UDF writer's `validate` may override a *zero* sector-byte option to **2048** (as QNX6 overrides defaults), while honoring an explicit 512 choice. For this format, allocation unit ordinarily equals logical block size; don't invent a FAT-like cluster size.

Potential additions only when they have a complete codec and tests: `udf_media` enum, `udf_duplicate_metadata`, `udf_uid_policy`, `udf_encode_normalization`, `udf_revision`; avoid generic untyped `-O key=value` semantics if the actual fsimg CLI does not expose them. If extending public options structs, increment `FSIMG_OPTIONS_VERSION` consistently and preserve the library's exact-version contract.

### 8.3 Example 2048-byte-sector layout, illustrative only

Consider a bounded 8 MiB closed image:

```
block size S = 2048 bytes
blocks N    = 8 * 1024 * 1024 / 2048 = 4096
last LBA    = 4095
anchor LBAs = 256, 3839, 4095   (where legal for profile)
```

Sketch (positions other than anchors are chosen by the writer's layout planner, **not UDF-mandated**):

```
byte 0..32767       non-ECMA reserved area, zero-filled by default
at byte 32768      VRS, e.g. BEA01/NSR02/TEA01 in 2048-byte records
block 256           primary AVDP
blocks 257..272     main VDS extent (at least 16 blocks)
blocks 273..        planned supporting metadata and partition start
...                 PD-defined physical partition:
                    space bitmap, FSD, root FE, file FEs, FIDs,
                    AEDs, data extents, free blocks
...                 LVID integrity extent and reserved volume metadata
blocks near end     reserve VDS extent (must not collide with AVDPs)
block 3839          redundant AVDP
block 4095          terminal AVDP if required/selected
```

Each reserved item has a distinct ownership claim, and the partition starts after the space used by those chosen external structures. There is no reason to fix one magic partition start in code: the plan can choose it after accounting for anchors, VDS, FSD and minimum media-specific requirements. For minimal images with no room for valid anchors and required metadata, return `FSIMG_ERANGE`/`FSIMG_ELIMIT` with a meaningful minimum volume size rather than emitting a truncated UDF.

For 512-byte sector images, VRS is still defined in 2048-byte recognition sectors beginning at 32 KiB, while anchors are located by **the volume's logical block number 256**, not by 2048-byte units. This distinction is a mandatory test case.

### 8.4 Planning records and geometry search

Plan structure (driver-owned, compact, scratch-backed):

```
udf_plan:
  version, media_profile, LBS, total_LBAs, volume_label
  reserved_ranges[bounded]
  VDS_main, VDS_reserve, LVID, physical_PD, FSD, root_ICB
  object_rows[] in scratch:
      catalog_id, parent_id, kind, size, unique_id,
      encoded_name_length, FID_size, FE_or_EFE_size,
      alloc_descriptor_kind, num_data_blocks, num_AEDs,
      planned_ICB_LBN, directory_data_LBN, source_id
  physical_extent_records[]
  free_space_bitmap_or_table
  checksums_and_terminal_descriptors
```

Multi-pass planning:

1. **Validate catalog and options.** Reject file names that are not representable as CS0 (or exceed 255 encoded bytes), unsupported kinds, symlink targets with invalid component decomposition, required unrepresentable metadata, and profile-incompatible sector sizes. Count directories and files without duplicating whole source names in RAM.
2. **Choose deterministic names, IDs and FE flavor.** Generate FID lengths and determine which objects can be inline based on exact FE/EFE header, EA and AD capacities. Avoid decisions based on source data contents unless the source contract explicitly provides ZERO ranges; don't read entire file just to decide inline eligibility.
3. **Profile directory byte streams.** For each directory, count parent FID, children FIDs, 4-byte alignment, optional padding and block-placement rules. Include the root directory's parent FID pointing back to root.
4. **Plan data extents.** Convert nonzero source ranges into block runs, consolidate adjacent runs, represent declared zero ranges either as sparse AD type 10 (where compatible) or physical zero ranges, apply max AD length and last-partial-block constraints. Compute needed AED chain capacity if ADs do not fit in FE/EFE.
5. **Plan metadata blocks.** FSD, one ICB per object, directory content, AEDs, bitmap/table and ancillary UDF descriptor areas. Estimate metadata *without* assuming all FE/AD entries fit in one sector for all sizes; a huge fragmented object has a separately bounded number of AEDs.
6. **Find volume size.** If caller supplies exact `volume_bytes`, check alignment and fits. Otherwise grow/shrink over valid block counts using deterministic first-fit (and the separately documented `-M` minimization policy when implemented), accounting for anchors near **both ends**, VDS duplication, free reserve and size of allocation bitmap. Avoid circular layout changes by computing the bitmap length from candidate partition blocks and iterating to a fixed point under explicit convergence bound.
7. **Assign physical addresses.** Reserve global descriptors, partition metadata and data; assign rows in stable catalog order. Ensure FSD/ICB/FID references are now final.
8. **Generate free-space bitmap.** Every physically allocated sector is bit zero; remaining legal free sectors are bit one. Cross-check accounting and sum LVID tables. If the generated free-space info depends on a descriptor's own allocated blocks, reserve them before bitmap generation, not afterward.
9. **Freeze a plan summary.** `fsimg_plan_info_t` counts payload bytes, initialized payload bytes, allocation bytes, metadata bytes, remaining free space, entries and any metadata loss. `emit` must not affect these numbers.

**Metadata partition writer:** follow the separate staging in §4.4, which expands steps 4–9 to two logical allocation domains and mirror physical backing. The baseline writer must not have conditional special cases for metadata unless `udf250` is selected.

### 8.5 Emission order and reproducibility

Common fsimg emission zero-initializes the entire sink. UDF emission should then fill only owned regions:

1. Encode and write file payload extents from source providers using common read/cancel/stability checks, zero-synthesis for declared zero regions and a verified size.
2. Write directory data including FIDs in canonical order; ensure root/parent FID correctness.
3. Write FE/EFE and AED records with fixed addresses and correct `TagLocation`/CRC/checksum.
4. Write bitmap or space tables, FSD and other physical/metadata structures.
5. Write PVD/PD/LVD/IUVD/USD/terminators into main and reserve VDS.
6. Write closed LVID, valid AVDPs and recognition sequence in all required positions (actual emission writes can be ordered differently so long as an incomplete output is never published).
7. Flush through the common output coordinator according to durability flags, then publish atomically according to existing fsimg rules.

The final image is deterministic given identical source tree contents/metadata, epoch, format settings and volume label. Check that all reserved bytes/padding are zero and no ambient local time, PID, temporary filename, filesystem inode, memory address or randomness leaks into the resulting image in reproducible mode. If a UDF volume identity would otherwise use randomness, define a deterministic replacement or require explicit identity.

**CRC operation:** encode descriptor body; compute payload CRC16; write length/CRC into the 16-byte tag; finally compute tag checksum while omitting its own checksum byte. The wire emission helper should do this identically for PVD/PD/LVD/AVDP/FSD/FID/FE/EFE/SBD/AED etc. Unit-test the helper on independently calculated vectors.

### 8.6 Fragmentation, sparse files, metadata and capacity decisions

* Zero-length file: emit zero `InformationLength`, no data extents, valid FE/EFE and FID.
* Small file: inline if bytes fit and profile supports it, including correct `LogicalBlocksRecorded=0`.
* Large file: physical extents limited by sector geometry and `0x3fffffff` field; emit multiple ADs and AED chain with no cycles. Data reads are bounded by logical size.
* Sparse/known-zero file: use unrecorded AD types where legal; source provider's zero regions are not silently discarded if their physical allocation status matters. Holes can save physical data but require a complete allocation/verification model.
* Huge directory: may not be inline, allocate directory stream blocks/AEDs as needed, maintain bounded memory.
* Unicode names: compute wire length before placing FIDs and reject collisions before writing.
* Metadata time: 12-byte UDF timestamps are at most microsecond precision; handle `required` and `ALLOW_LOSS` before emitting.
* Minimum free space: apply to the partition's *allocatable blocks*, not volume-block total that includes anchors and VDS.
* Very large image: use `uint64_t` for byte arithmetic, but honor 32-bit LBN fields, 32-bit sector counts and each map's representable partition lengths. A 64-bit file offset does **not** make a 32-bit UDF LBN magically support that volume size.

## 9. Verification contract and security model

### 9.1 fsimg validation stages

| Operation | Minimum checks | Must not claim |
|---|---|---|
| `probe` | bounded VRS identity | filesystem consistency, readable UDF revision |
| `reader.open` / `info` | anchors, VDS, LVD maps, FSD, root ICB, valid fields and addresses needed to open | full allocation correctness, all file payloads readable |
| `ls` / `next` | FID record framing, CS0 names, namespace duplicates, root/parent/child relationships, FE/EFE for encountered objects | entire partition bitmaps consistent |
| `cat` / extract | every requested extent type, physical mapping, holes, content bounds, source identity | all unvisited objects consistent |
| `verify` | complete reachable namespace, AED graphs, physical allocation ownership, free-space tables/bitmaps, LVID, revision constraints, copies/mirrors and link counts | data bytes match an absent application-level digest |
| `verify` with payload | as above plus read every initialized recorded file byte | contents have an independent cryptographic integrity hash (UDF has no universal per-file content hashes) |

Use fsimg's `FSIMG_VERIFY_PAYLOAD` flag for the last stage. `fsimg_volume_info.scope` should identify whether only header or full verification was achieved. The UDF closed integrity bit is not a substitute for `verify`.

### 9.2 Threat model

All image bytes, filenames, extent references, indirection structures, table counts, CRC fields and optional unknown extensions are adversarial. This is a **format parser in a reusable C library**; one malformed FID or VAT must never produce out-of-bounds read, allocation explosion, infinite loop, host escape, use-after-free, or unchecked write.

Mandatory defenses:

1. **Address space checks:** range validate every logical-to-physical transformation and reject overflow before byte multiplication. Respect `fsimg_io_slice` and physical domain.
2. **Descriptor size checks:** tag checksum + CRC + tag location, then independently derived record sizes. No unvalidated cast or flex-array length. Decode in small fixed buffers where possible.
3. **Finite graph traversal:** separate visited sets for VDS pointers, VAT predecessor chain, metadata-file backing, AED continuations and directory ancestry. Hard links are legal repeated FE references, but directory ancestor cycles are not.
4. **Resource charging:** work per parsed descriptor, FID, Unicode code unit, VAT map page, sparing lookup, metadata fallback and file extent; enforce memory/scratch/depth/path budgets. An image's self-declared number of entries is not a permit to allocate that many immediately.
5. **Atomic object visibility:** complete validation of an entry's FID, name and FE/EFE precedes exposure to the common walker. A short metadata read is a failure, not a partially usable record.
6. **No untrusted inline devices:** never create host character/block devices from file types 6/7. Existing extractor safe/no-follow rules apply to symlinks as well.
7. **Zero-filled holes:** never return unrecorded-sector bytes through an accidental DATA extent.
8. **Overlapping reservations:** no regular file data may claim VRS, anchor, VDS, FSD, space bitmap, metadata backing or a different object's allocated blocks except documented logical aliases.
9. **Unknown features:** unknown mandatory partition-map/entity ID/AD mode/strategy is `EUNSUPPORTED` and remains bounded; no speculative fallback to a Type-1 map.
10. **Source stability:** the input image remains stable across traversal. Existing fsimg host-identity/stability checks and terminal-failure behavior are used; no stale cached mapping after source mutation.
11. **Error provenance:** fill `fsimg_error_t` with operation, offset and reason. For a logical address, include map/ref/LBN in the message and the resolved image offset when available. Do not leak unchecked formatted values or attacker-driven uncontrolled string lengths into logs.
12. **Fuzzer hygiene:** cap decompressed/expanded output for malformed CS0, prevent time-related unchecked arithmetic, and bound recursive symlink-component lengths.

### 9.3 Error taxonomy

| Condition | Expected fsimg status |
|---|---|
| No UDF VRS at expected recognition region | `FSIMG_EFORMAT` |
| Valid UDF signature but unknown required partition-map kind or nonrepresentable entry kind | `FSIMG_EUNSUPPORTED` |
| CRC/tag checksum failure, contradictory block geometry, out-of-range AD, FID length overrun, illegal cycle | `FSIMG_ECORRUPT` |
| Size/capability option impossible, name/label rejected at writer plan | `FSIMG_EINVAL` / `FSIMG_ERANGE` as appropriate |
| Budget exhausted on valid but huge image | `FSIMG_ELIMIT` |
| Underlying read/write failed | `FSIMG_EIO` |
| Source changed during content read | `FSIMG_ESOURCE_CHANGED` |
| User cancellation | `FSIMG_ECANCELLED` |
| Durable publication cannot be guaranteed | `FSIMG_EDURABILITY` |

A **valid unsupported** profile is not a corrupt filesystem. A bad CRC inside an otherwise recognized descriptor is corrupt, not simply an unknown format. Avoid letting parser failure accidentally fall through to ISO 9660 after UDF was claimed.

### 9.4 Directory/content lazy work: what to validate when

At entry exposure: FID framing, Unicode decode, FE/EFE identity, kind, file size and metadata fields; ensure AD list framing is valid enough not to hide impossible lengths. At first read/seek: resolve extent chain and ensure every returned physical run is bounded; cache indexed extents if necessary. At full verify: traverse all chains, build ownership summary, reconcile bitmaps, duplicates, link counts and volumes' redundant structures. This is consistent with fsimg's existing lazy walker contract; do not force whole-image verification for `info`.

### 9.5 Performance and complexity targets

Let `D` be total directory FIDs, `A` allocation descriptors, `M` partition mapping entries touched, `B` number of physical blocks. Expected efficient profile:

* `info`: O(number of VDS/partition descriptors + small mapper bootstrap), not O(B).
* `ls -r`: O(D + metadata AD traversal + directory bytes), disk access proportional to visited records, not B.
* `cat` sequential: O(file bytes + file ADs + map lookups); cache last/next AD to avoid rescanning from file start for each read.
* random file reads: O(log A) with spillable AD index, plus one or a small number of map lookups; never O(A) per sector.
* `verify`: O(D + A + B/8 + sort/merge cost) with scratch-backed physical ownership and bitmap passes. A block-ownership sort can be O(A log A) externally. No O(B) RAM allocation.
* Large VAT: paged lookup into `fsimg_io`, scratch backed index or sparse bounded cache; a million-entry VAT must not require unbounded heap.
* Metadata mirror: ordinary read uses primary; verify cross-checks mirror as an explicit additional scan, not implicit duplicate reading of every data access.

Spare table lookups can binary-search sorted packet pairs or use a small scratch-backed search index; do not linearly rescan an unsorted table for every sector.

## 10. Independent implementation references and license classifications

**License boundary:** 'use for implementation' below means it may be studied and, after **per-file** license audit, suitable original/permissively licensed elements can be adapted consistent with notices. It does *not* grant permission to copy unrelated standard text or non-permissive transitive files. 'Oracle only' means **do not copy code** and **do not vendor/link** into the BSD-2-Clause fsimg source tree; execute a separately installed program in test environments and compare output.

### 10.1 Permissively licensed implementations — first choice

| Project/file | License evidence | Valuable components | Priority |
|---|---|---|---|
| **NetBSD kernel UDF:** [sys/fs/udf/](https://github.com/NetBSD/src/tree/trunk/sys/fs/udf) | BSD-family, check each file's header | Broad read/write architecture, VAT/metadata/sparing, directory traversal, alloc descriptor iteration, optical strategies | **Highest for semantics** |
| **NetBSD ECMA/UDF structures:** [ecma167-udf.h](https://github.com/NetBSD/src/blob/trunk/sys/fs/udf/ecma167-udf.h) | BSD 2-clause-style headers on inspected file | Standard numeric fields and `tag`/FE/EFE/VAT/metadata layouts | **Highest for wire facts** |
| **NetBSD image-tree writer:** [usr.sbin/makefs/udf.c](https://github.com/NetBSD/src/blob/trunk/usr.sbin/makefs/udf.c) | Inspected copyright header: BSD 2-clause-style | **Populate filesystem image from a directory tree**, writer planning, UDF file placement and metadata | **Highest for fsimg writer** |
| **NetBSD UDF formatter:** [sbin/newfs_udf/](https://github.com/NetBSD/src/tree/trunk/sbin/newfs_udf) and [udf_core.c](https://github.com/NetBSD/src/blob/trunk/sbin/newfs_udf/udf_core.c) | Inspected BSD 2-clause-style | Geometry, physical layout, metadata partition, optical-media mappings, descriptor generation | **Highest for formatter** |
| **NetBSD UDF checker:** [sbin/fsck_udf/](https://github.com/NetBSD/src/tree/trunk/sbin/fsck_udf) | Inspected BSD 2-clause-style | Verification of allocation graphs, VAT and metadata, recovery behavior | **Highest for validation** |
| **FreeBSD UDF:** [sys/fs/udf/](https://github.com/freebsd/freebsd-src/tree/main/sys/fs/udf) and [ecma167-udf.h](https://github.com/freebsd/freebsd-src/blob/main/sys/fs/udf/ecma167-udf.h) | Inspected struct header has `SPDX-License-Identifier: BSD-2-Clause` | Smaller reader, basic UDF/ECMA-167 traversal, endian utilities | Good small baseline |
| **OpenBSD UDF:** [sys/isofs/udf/](https://github.com/openbsd/src/tree/master/sys/isofs/udf) | BSD-family; inspect each individual source file | Independent BSD reader to resolve conflicting interpretation | Good cross-check |
| **NetBSD user-space makefs frontend:** [makefs.c](https://github.com/NetBSD/src/blob/trunk/usr.sbin/makefs/makefs.c), [makefs manual](https://man.netbsd.org/makefs.8) | BSD-family with per-file verification | Reusable image-building test generator, host-tree semantics | **Excellent reference/oracle** |
| **`mogaika/udf`**: [Go read-only UDF parser](https://github.com/mogaika/udf) | Repository declares BSD-3-Clause | A compact independent parser model, useful small comparison | Secondary only; README warns functionality is broken for some images |
| **`Randrianasulu/newfs_udf-linux`**: [NetBSD port](https://github.com/Randrianasulu/newfs_udf-linux) | Repo declares BSD-2-Clause; audit imported files separately | Linux regular-file formatter where NetBSD VM is unavailable | Secondary; small experimental port |

**Important inherited-code warning:** some UDF source trees include Unicode compression routines historically imported from OSTA samples, e.g. parts of `udf_osta.c`; a BSD license on a directory or umbrella repository does **not** prove every imported function is independently BSD. Treat all such files as **requiring individual provenance review**, and preferably write fsimg's tiny CS0 codec from the standard's defined byte encoding rather than importing it.

**Recommended code-study order:** NetBSD `makefs/udf.c` for complete-tree population → NetBSD `newfs_udf/udf_core.c` for correct descriptor placement → NetBSD `sys/fs/udf/udf_subr.c`/`udf_allocation.c`/`udf_strat_*.c` for partition translation → NetBSD `fsck_udf/main.c` for invariant checking → FreeBSD/OpenBSD for minimal reader contrast. They solve complementary problems; none should be used uncritically as an unquestioned oracle.

### 10.2 Non-permissive code — **external test tools only**

| Project | License boundary | Why it's useful |
|---|---|---|
| [udftools](https://github.com/pali/udftools): `mkudffs`, `udfinfo`, `udflabel`, `udffsck`, `wrudf` | GPL-2.0-family; do not import | Linux volume formatter, descriptor inventory, repair/check tests, media/VAT variants |
| [Linux UDF kernel driver](https://github.com/torvalds/linux/tree/master/fs/udf) | GPL-2.0; do not import | Kernel read/optional write behavior and test oracle; known limits do not define the standard |
| [VideoLAN libudfread](https://code.videolan.org/videolan/libudfread) / [mirror](https://github.com/firecore/libudfread) | LGPL-2.1-or-later in inspected implementation | Independent parser used in media software; external helper/program only |
| [UDFclient toolkit](https://www.13thmonkey.org/udfclient/) / [Debian source](https://sources.debian.org/src/udfclient/) | **Clarified Artistic License** on toolkit components; not blanket BSD-approved | File listing, copy in/out, session operations, low-level inspection |
| [7-Zip](https://www.7-zip.org/) | Mixed LGPL/BSD and other conditions | Independent **read-only** UDF archive extractor/list/test; do not copy parser |
| [GNU libcdio](https://github.com/libcdio/libcdio-C) | GPL-3.0-family overall; check individual files only if separately licensed | Optical UDF/ISO information tooling; external oracle |
| [genisoimage](https://manpages.debian.org/bookworm/genisoimage/genisoimage.1.en.html) | GPL family | Generates **hybrid ISO/UDF**, not a standalone general UDF writer |
| [pycdlib](https://github.com/clalancette/pycdlib) | LGPL-2.1 in project sources | Python-based ISO 9660/UDF bridge generation and name-view comparison |
| Apple's `hdiutil makehybrid` | Proprietary executable, public manual | macOS-created ISO/UDF bridges; **use as a black-box tool, not a code source** |
| Microsoft Windows filesystem drivers / `format` | Proprietary implementation | Windows read/write interoperability and UDF 2.50 duplicated metadata behavior; black-box tests |

### 10.3 Related standards, guides and historical implementations

* [ECMA TR/112 landing](https://ecma-international.org/publications-and-standards/technical-reports/ecma-tr-112/) — all major revisions and Secure UDF.
* [ECMA-167](https://ecma-international.org/wp-content/uploads/ECMA-167_3rd_edition_june_1997.pdf) — volume structure and file structure normative basis.
* [NetBSD `newfs_udf(8)`](https://man.netbsd.org/newfs_udf.8) — media profiles and image creation.
* [NetBSD `makefs(8)`](https://man.netbsd.org/makefs.8) — native directory-tree image creator with UDF target.
* [NetBSD `fsck_udf(8)`](https://man.netbsd.org/NetBSD-10.2/fsck_udf.8) — filesystem consistency checker and VAT recovery options.
* [NetBSD `mount_udf(8)`](https://man.netbsd.org/mount_udf.8) — kernel mount support and flags.
* [udftools `mkudffs(8)`](https://man7.org/linux/man-pages/man8/mkudffs.8.html) — revision/media/blocksize/space/AD options. Revisions beyond 2.01 are described as experimental by the tool; don't treat `--udfrev=2.60` as a full conformance guarantee.
* [udftools `udfinfo(1)`](https://man7.org/linux/man-pages/man1/udfinfo.1.html) — machine-friendly descriptor info and block-range map.
* [7-Zip supported unpack formats](https://www.7-zip.org/) — includes UDF, unpack only.
* [macOS `hdiutil(1)`](https://keith.github.io/xcode-man-pages/hdiutil.1.html) — `makehybrid`, `-udf` and `-udf-version` (1.02 and 1.50).
* [Microsoft `format` documentation](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/format) — UDF formatting on supported drives; `/D` duplicates UDF 2.50 metadata.
* [FreeBSD storage handbook UDF hybrid example](https://docs.freebsd.org/en/books/handbook/disks/) — `mkisofs -R -J -udf -iso-level 3` and disc writing workflow.
* [Linux `mount(8)`](https://man7.org/linux/man-pages/man8/mount.8.html) — userspace loop setup for out-of-process tests. No mount required in the fsimg library.
* [OSTA](https://www.osta.org/specs/) — standards history/industry context; prefer current Ecma copies for normative work.

## 11. Independent test tools: read, create, inspect, verify

### 11.1 Test-tool matrix

| Tool | Environment | Create empty volume | Populate files / write | List/extract | Verify/check | Best role |
|---|---|---:|---:|---:|---:|---|
| **NetBSD `makefs -t udf`** | Supported NetBSD release / VM | Yes | **Yes, directly from directory tree** | — | — | Golden reference **writer**, independent fsimg tree population (currently up to 2.50, not 2.60) |
| **NetBSD `newfs_udf`** | NetBSD/VM | **Yes** | No by itself; mount or `udfclient` to add | — | — | Volume geometry, UDF 2.50 backing variations |
| **NetBSD `fsck_udf`** | NetBSD 10+ | — | Repair with explicit user approval (not tests) | — | **Yes, read-only with `-n`** | Independent deep checker |
| **NetBSD kernel `mount_udf`** | NetBSD/VM | — | RW when supported | **Yes** | Mount is *not* fsck | Native OS compatibility |
| **Linux `mkudffs`** | Linux | **Yes** | Doesn't populate arbitrary directory trees | — | — | Revision/media/geometry variants, empty images |
| **Linux `udfinfo`** | Linux | — | — | Descriptor info incl. `--startblock/--lastblock/--vatblock` | **Partial only** | Independent headers/partition map/count oracle |
| **Linux `udffsck`** | Linux | — | May repair; use read-only mode if available | — | **Partial/incomplete** | Secondary checker, not single truth source |
| **Linux UDF kernel via read-only loop mount** | Linux VM | — | Write in disposable private mount when supported | **Yes** | Not full structural verify | Readability, metadata/content checks |
| **`udfclient`** | Unix-like | `newfs_udf` variant in toolkit | **Yes** on supported writable images | **Yes** | Structural information, not full fsck | User-mode oracle with no OS mount |
| **7-Zip / p7zip 7z** | Windows/Linux/macOS packages | — | **No UDF creation** | **Yes** | File-data test, not UDF fsck | Easy cross-platform data reader |
| **macOS `hdiutil makehybrid -udf`** | macOS | **Yes** | **Yes** from source tree | macOS mounts UDF | — | Independent UDF/ISO bridge generator |
| **macOS `newfs_udf`** | macOS editions where available | **Yes** | Mount to add files | OS-dependent | — | Additional formatter profile |
| **`genisoimage -udf` / `mkisofs -udf`** | Linux/BSD | **Hybrid only** | **Yes** | — | — | ISO/UDF bridge fixtures, shared-data tests |
| **`pycdlib`** | Python test runner | **Hybrid image** | Yes through its ISO/UDF APIs | Programmatic | Partial semantic check | Alternate bridge generator/parser |
| **Windows Explorer / native UDF** | Windows VM | On supported media | Yes on supported writable media | **Yes** | OS-level checks | Cross-OS file/dir/name/timestamp behavior |

**Do not use `udfinfo` or `7z t` alone as a 'clean' verdict.** They confirm that a selected subset can be decoded, not that every reachable allocation and redundant structure is consistent.

### 11.2 Verified/safe command recipes

All commands below operate on **regular files** unless a step explicitly mentions a VM-mounted device. Avoid unreviewed `/dev/sdX` or optical-device paths.

**Linux: create basic reference UDF volume** (udftools 2.x):

```sh
# Work in a disposable test directory. Order of --media-type and --udfrev matters.
mkudffs --new-file --blocksize=2048 --media-type=dvd \
  --udfrev=1.02 --label=UDFTEST udf102.img 16384
udfinfo udf102.img

# A 2.01 HD-style image exercises a different media profile.
mkudffs --new-file --media-type=hd --blocksize=512 \
  --udfrev=2.01 --label=UDFHDD udf201.img 65536
udfinfo --blocksize=512 udf201.img

# These options are useful for checking a plan without writing to the image.
mkudffs --no-write --media-type=dvd --udfrev=1.02 \
  --blocksize=2048 udf102.img
```

The `blocks-count` argument is **in blocks**, not bytes. For `mkudffs --new-file`, verify exact installed version/exit status and delete the disposable output before rerunning. `--no-write` may inspect an existing file; do not use an output name that matters.

**Multisession/VAT-specific `udfinfo` options** (udftools 2.3+, for a real known recorded-session geometry):

```sh
# session-start, session-last, and vat-lba come from a captured media/TOC
# manifest. They are NOT inferred or guessed from a filename.
udfinfo --blocksize=2048 --startblock=SESSION_START \
  --lastblock=SESSION_LAST --vatblock=VAT_LBA session.udf
```

The tool's manual explicitly warns that a **flat file cannot reveal the original optical disc Table of Contents**, and therefore UDF VAT/multisession images may need the `--startblock`, `--lastblock` and `--vatblock` values. Preserve those facts as fixture metadata and give the fsimg reader a bounded missing-session-information result, not a false empty filesystem. NetBSD `fsck_udf -S` searches older VATs and `-s` selects historical sessions; these are especially useful **read-only** recovery test oracles when combined with `-n -f`.

**NetBSD: create a populated UDF image, without root privileges**:

```sh
mkdir -p tree/sub
printf 'hello UDF\n' > tree/hello.txt
printf 'nested\n' > tree/sub/child.txt

# Check NetBSD makefs(8) for this release's supported -o keys.
makefs -t udf -o disctype=dvdrom -o minver=0x102 \
  udf-netbsd.iso tree

# The formatter alone makes an empty image (example from NetBSD manual).
newfs_udf -L bigdisk2 -F -s 200M -S 2048 empty-netbsd.iso

# On NetBSD 10+ inspect with an independent checker without repair.
fsck_udf -fn udf-netbsd.iso
```

`makefs`'s `disctype` and revision options vary by branch and supported media; pin the OS release and capture `makefs -t udf -h`/man page in test manifest. On a NetBSD release where `newfs_udf` requires a preexisting image file or a specific `-F` policy, use its documented image command. `fsck_udf -n -f` is a *check* mode; never pass `-y` in automated fixtures.

**macOS: hybrid source from a directory**:

```sh
mkdir -p test-tree/sub
printf 'example\n' > test-tree/sub/hi.txt
hdiutil makehybrid -udf -udf-version 1.02 \
  -o mac-udf.iso test-tree
# For an ISO/UDF bridge, intentionally request both views:
hdiutil makehybrid -udf -iso -joliet -udf-version 1.50 \
  -o mac-bridge.iso test-tree
```

`hdiutil makehybrid -udf` is documented for 1.02 and 1.50 only, not 2.50/2.60. The output may have a `.iso` extension even if UDF is present; always inspect its contents, not the filename. Some hybrid commands and defaults differ across macOS versions.

**Additional generated-bridge oracle: pycdlib.** The [pycdlib UDF example documentation](https://github.com/clalancette/pycdlib/blob/master/docs/example-creating-udf-iso.md) describes constructing an ISO/UDF bridge with a selectable UDF revision (including `udf='2.60'`) and separate UDF/ISO paths. This is particularly valuable for a fixture where the *same payload is reachable under different names or hierarchies*. pycdlib is LGPL; invoke it as an external Python development/test dependency and **do not import its source** into fsimg. Its UDF-2.60 output is a bridge interop fixture, not evidence of general BD-R/POW media conformance.

**ISO/UDF bridge via mkisofs/genisoimage**:

```sh
mkisofs -R -J -udf -iso-level 3 -o bridge.iso test-tree
# Alternative if mkisofs command is supplied as genisoimage:
genisoimage -R -J -udf -iso-level 3 -o bridge.iso test-tree
```

For the same bridge image, test fsimg's forced `-t udf` independently from `-t iso9660` and ensure results don't overwrite each other's namespace choices. Modern `genisoimage`'s UDF path is limited; this is **a bridge fixture**, not proof of UDF 2.60 conformance.

**Generic file read/test with 7-Zip (supports UDF unpack)**:

```sh
7z l -slt udf-netbsd.iso
7z t udf-netbsd.iso
mkdir -p via-7z
7z x -y -ovia-7z udf-netbsd.iso
```

This is a convenient cross-platform secondary oracle, especially when a kernel rejects a valid unusual profile. It does **not** inspect all free-space bits or redundant anchors.

**Linux UDF kernel read oracle, only inside a disposable VM/container with permission**:

```sh
mkdir -p /mnt/udf-test
sudo mount -t udf -o loop,ro,nosuid,nodev,noexec udf-netbsd.iso /mnt/udf-test
find /mnt/udf-test -type f -print0 | sort -z | xargs -0 -r sha256sum
sudo umount /mnt/udf-test
```

This test depends on kernel loop/UDF support and a permitted mount namespace; **it is not a requirement to build or run fsimg**. An OS accepting a UDF image is evidence of interoperability, not proof of formal conformance. Prefer NetBSD and Linux independent mounts plus 7-Zip for cross-checking.

**Windows VM:** use File Explorer/PowerShell to mount the image when supported, compare file trees/hashes and metadata, and record mounted UDF revision and errors. Microsoft's `format /FS:UDF` is destructive on a **selected drive or removable device**, not a casual raw-image creator: reserve it for strictly disposable VM test volumes and confirm the target. `/D` explicitly requests UDF 2.50 metadata duplication on supported paths.

### 11.3 An executable oracle comparison strategy

For a legal reference image, run:

```sh
# fsimg commands shown as representative existing CLI spelling; check exact
# flags against the checked-out binary if interfaces have changed.
fsimg info -i ref.udf
fsimg ls -i ref.udf
fsimg verify -i ref.udf
fsimg cat -i ref.udf path/to/file > fsimg.out
sha256sum fsimg.out
udfinfo ref.udf
7z t ref.udf
```

Compare to a *source-tree manifest*, not just one implementation's directory listing. Manifest fields: normalized path, file kind, byte length, SHA-256 (for regular files), symlink target, UID/GID, mode, A/M/C times with allowed precision/zone deviations, hidden/extra streams, and supported profile. Directory listing order is not necessarily a semantic equality condition; fsimg's deterministic writer ordering is tested separately.

For a created fsimg image, reverse the direction:

```
source tree -> fsimg write -> fsimg verify -> NetBSD fsck_udf -fn
                                 -> Linux udfinfo
                                 -> 7z test/extract
                                 -> independent Linux and NetBSD read-only mounts
                                 -> file hashes + metadata comparison
```

**Important gate:** `mkudffs` creates an *empty* filesystem; using it alone cannot validate fsimg's FID/FE/EFE data population. **NetBSD `makefs -t udf`** and `hdiutil makehybrid` are the strongest independent, no-mount sources for already-populated images. NetBSD `fsck_udf` is the strongest separate on-disk consistency checker; use all three where practical.


## 12. Specification-driven tests, fixture corpus and qualification

This section is intended to feed fsimg's existing Ruby 3.2 `test/spec/` runner and C/libFuzzer suites. External tools are independent *process* oracles and never libraries linked into `libfsimg`.

### 12.1 Fixture dimensions and expected provenance

Each `udf` fixture has a sidecar metadata record with:

```json
{
  "schema": 1,
  "format": "udf",
  "udf_revision": "2.50",
  "media_profile": "readonly",
  "sector_bytes": 2048,
  "logical_block_bytes": 2048,
  "generator": "netbsd-makefs",
  "generator_version": "11.99",
  "generator_command": "...",
  "license_provenance": "fixture generated from project-owned tree",
  "tree_sha256_manifest": "...",
  "features": ["efe", "metadata_partition", "unicode"],
  "expected": {
    "info": "accept",
    "list": "accept",
    "verify": "accept",
    "payload": "accept"
  },
  "oracle_versions": {
    "netbsd_fsck_udf": "...",
    "linux_kernel": "...",
    "7zip": "..."
  }
}
```

Values `"..."` indicate fields to be *filled in from actual commands/results*, not fabricated reference results. The fixture provenance is essential: `7z` and `udfinfo` may both accept images with inconsistent allocation bitmaps.

Create a **cross-product where practical**, not an unbounded Cartesian expansion:

| Axis | Representative fixtures |
|---|---|
| Revision | 1.02, 1.50, 2.00, 2.01, 2.50, 2.60; 1.01 optional reader-only history |
| Logical block size | 512, 1024, 2048, 4096; 8192–32768 optional reader extensions |
| Media | DVD-ROM, HD, CD-R(VAT), CD-RW(sparing), BD-ROM(metadata), BD-R(POW), DVD bridge |
| Map | physical Type-1, virtual Type-2/VAT, sparable Type-2, metadata Type-2, valid multi-map combinations |
| Entry kind | directory, regular, inline file, empty file, symlink, FIFO, device/socket unsupported |
| Allocation | short, long, inline, AED continuation, recorded, allocated-unrecorded, unallocated hole |
| Names | ASCII, mixed case, non-ASCII BMP, supplementary-plane scalar, near limit, duplicates/invalid CS0 |
| Metadata | absent/unspecified owner, POSIX owner and mode, precision/timezone edge, creation and EA, named stream |
| Topology | 1 entry, deep path, wide directory, hardlinks, large file, discontiguous data, huge map/bitmap |
| Redundancy | main/reserve VDS good/bad, anchors missing, metadata mirror failover, sparing table disagreement |
| Medium shape | exact image length, truncated tail, too small for anchor, slice offset, sparse image, appended session |

Do not ship copyrighted disc images or third-party proprietary application payloads in the repository. Prefer project-generated tiny files, build-on-test recipes, and legally redistributable byte patterns. Actual optical discs can be tested as local, untracked samples with hashes and documented permitted provenance.

### 12.2 Reader functional matrix

The first value below is the expected result **once the stated feature is implemented**. Before that milestone, an otherwise valid image requiring the feature must return `FSIMG_EUNSUPPORTED` rather than `EFORMAT`, crash, or bogus contents.

| ID | Fixture / operation | Expected property |
|---|---|---|
| R001 | Empty 1.02 volume | Detect, report geometry, enumerate root, verify empty allocations |
| R002 | Root with 1 ordinary file | FE/FID identity, exact payload hash |
| R003 | Nested directory chain | Descend once per path, no spurious cycles |
| R004 | Empty file and empty directory | Exactly zero logical size, zero payload bytes |
| R005 | Small inline regular file | Extract embedded bytes, no data-block ownership |
| R006 | Inline directory FIDs | Enumerate without treating FE bytes as a sector extent |
| R007 | Short AD fragmented file | Correct seek and partial reads across extents |
| R008 | Long AD mapped file | Resolve map index, not physical PD number |
| R009 | AED continuation | Read content in both first and later AD lists |
| R010 | Allocated-not-recorded middle span | Return zeros without reading physical slack; mark allocated ownership |
| R011 | Not-allocated middle span | Return zeros without claiming a physical block |
| R012 | Recorded data after hole | Never stop at first hole or shift later offsets |
| R013 | Non-block-aligned file tail | Return only logical length; don't disclose slack |
| R014 | Single file >4 GiB | Correct 64-bit `InformationLength` and offset seek |
| R015 | UDF 1.50 VAT root | Find latest VAT, map virtual ICBs, check data contents |
| R016 | Historic VAT variant | Parse revision-specific old VAT structure and predecessor safely |
| R017 | Sparable table active remap | Translate packet plus within-packet offset |
| R018 | 2.50 metadata partition | Read FSD/directory/FE via Metadata File and payload via physical map |
| R019 | Independent metadata mirror | Return primary data; compare replicas in full verify |
| R020 | Metadata primary corrupted, valid mirror | If recovery supported, safe fallback with degraded report, verify failure |
| R021 | 2.60 pseudo-overwrite image | Recognize/map supported snapshot; don't claim write support |
| R022 | ISO/UDF bridge | Both format signatures; forced UDF selects UDF hierarchy |
| R023 | CS0 compression ID 8 | Valid UCS-2 scalar mapping to UTF-8 |
| R024 | CS0 compression ID 16 | Correct endian and surrogate/unsupported code point policy |
| R025 | Mixed ASCII/Unicode names | Byte-exact expected Unicode names and file sizes |
| R026 | Case-distinct names `A` and `a` | Coexist where legal; no FAT-style case collision |
| R027 | Parent FID | Not displayed as child; matches referencing rules |
| R028 | Deleted FID with alternate compression | Skip safely; retain allocation checks where required |
| R029 | Several FIDs to one file FE | Hardlink content agrees, links counted without directory cycle |
| R030 | Symlink component stream | Exact target under fsimg's stated supported 1023-byte target limit |
| R031 | EFE creation time | Decode and expose without conflating attribute time |
| R032 | Named stream directory | Validate internal structure; expose only if API extended |
| R033 | UDF 512/1024/4096 images | Probe/or open correctly after geometry validation |
| R034 | UDF image in nonzero-offset slice | Relative image locations all translated exactly once |
| R035 | Same file sequential vs random reads | Identical output and bound work |
| R036 | Input modified after open | Fail with source-change semantics, not stale UDF mapper cache |

### 12.3 Corruption and unsupported-feature mutation matrix

**Mutate one property at a time** starting from a demonstrably valid legal base. A bare bit flip that changes a descriptor body without fixing its checksum only tests CRC detection; for semantic validation, mutate the property *then recompute the tag checksum and CRC correctly*. Both varieties are needed.

| ID | Corruption / unsupported feature | Expected stage / failure |
|---|---|---|
| M001 | No VRS `NSR02`/`NSR03` | Probe `EFORMAT` |
| M002 | Only `CD001`, no UDF VRS | Probe `EFORMAT` |
| M003 | Correct VRS but absent anchors | Open `ECORRUPT` |
| M004 | Anchor tag-checksum byte wrong | Open `ECORRUPT` or safe valid-copy fallback with full verify failure |
| M005 | Correct tag checksum but wrong CRC16 | Same |
| M006 | Tag's declared CRC length beyond record | `ECORRUPT` |
| M007 | Correct CRC but wrong TagLocation | `ECORRUPT` |
| M008 | Reserve/main VDS contradictory revisions | `ECORRUPT` / clearly reported degraded fallback |
| M009 | VDS pointer cycle | `ECORRUPT` within fixed work budget |
| M010 | LVD map bytes shorter than declared lengths | `ECORRUPT` |
| M011 | Partition reference index outside map table | `ECORRUPT` |
| M012 | Map describes unsupported new semantics | `EUNSUPPORTED` |
| M013 | Physical partition past end-of-image | `ECORRUPT` |
| M014 | Partition-relative LBN multiplication overflow | `ECORRUPT` |
| M015 | FE/EFE EA length + AD length exceeds block | `ECORRUPT` |
| M016 | AD list has positive gap or excess beyond InformationLength | `ECORRUPT` |
| M017 | AD points into volume-descriptor sequence | `ECORRUPT`; never leak descriptor bytes as payload |
| M018 | AED points back to itself | `ECORRUPT` with finite traversal |
| M019 | AED chain repeated after many hops | `ECORRUPT` or `ELIMIT` at work cap |
| M020 | FID name length > available bytes | `ECORRUPT` |
| M021 | FID implementation-use length overflow | `ECORRUPT` |
| M022 | FID tag valid but target is wrong file type | `ECORRUPT` |
| M023 | FID decoded name contains separator/NUL | `ECORRUPT` / explicit unrepresentable-name error |
| M024 | Invalid CS0 ID, UTF-16 unit or malformed surrogate | `ECORRUPT` or `EUNSUPPORTED` per documented case |
| M025 | Two different encodings decode to identical name | `ECORRUPT` on directory prepare |
| M026 | Parent FID name unexpectedly nonempty | `ECORRUPT` |
| M027 | Directory cycle to root or ancestor | `ECORRUPT` within depth budget |
| M028 | FE link count inconsistent with FIDs | Full verify `ECORRUPT` |
| M029 | LVID says free when bitmap says owned | Full verify `ECORRUPT` |
| M030 | File/FSD/bitmap overlaps unrelated claimed block | Full verify `ECORRUPT` |
| M031 | VAT map index beyond VAT size | `ECORRUPT` |
| M032 | VAT maps to a block beyond physical partition | `ECORRUPT` |
| M033 | VAT ICB refers back to VAT-mapped region in bootstrap | `ECORRUPT`/`EUNSUPPORTED` with finite traversal |
| M034 | VAT last-tail signature forged with bad tag | No false latest-VAT acceptance |
| M035 | Sparing duplicate origin packet | `ECORRUPT` |
| M036 | Sparing packet length 0, overflow or misalignment | `ECORRUPT` |
| M037 | Sparing copies disagree | Full verify fails; reader does not mix arbitrary entries |
| M038 | Metadata backing FE references metadata map recursively | `ECORRUPT` |
| M039 | Metadata bitmap describes too few blocks | `ECORRUPT` |
| M040 | Metadata mirror overlaps unrelated content | Full verify `ECORRUPT` |
| M041 | Required 2.50 metadata map absent in a selected required profile | `ECORRUPT` or unsupported incompatible profile; no false conformance claim |
| M042 | UDF 2.60 unknown mandatory media feature | `EUNSUPPORTED` |
| M043 | Valid UDF realtime file (type 249) | Recognize but do not read or emulate regular data; `EUNSUPPORTED` |
| M044 | Hostile 64-bit file length with tiny image | No giant allocation; `ECORRUPT` or `ELIMIT` by measured cause |
| M045 | Descriptor counts/path lengths past fsimg budgets | `ELIMIT` without resource exhaustion |
| M046 | Truncated image after last readable data file | Full verify fails; never publish as valid |
| M047 | Sparse underlying input with holes | Input image holes read zero, but UDF allocation still validated |
| M048 | Tag serial/chosen mirror mismatch | Valid-copy recovery, if defined, never silently considered verified |

`ECORRUPT` vs `EUNSUPPORTED` must be decided per rule **before** writing tests. A test expecting an error only because a decoder hasn't been implemented should be converted to acceptance when that feature is completed; keep these cases as explicit capability gates.

### 12.4 Writer qualification matrix

| ID | Source/setting | Writer and independent-reader check |
|---|---|---|
| W001 | Empty tree, UDF1.02 default | Exact size, redundant anchors/VDS, root and closed LVID |
| W002 | Empty/one/two files | Correct FID/FE/bitmap and no unexpected aliases |
| W003 | Nested tree with empty dirs | Parent FIDs and directory sizes/counts |
| W004 | ASCII + 8-bit CS0 + 16-bit CS0 names | External filename round trip |
| W005 | Many short entries | Stable deterministic sort and unique IDs |
| W006 | A single huge directory | AED/list grows without unbounded RAM |
| W007 | Random 4 GiB+ file (sparse generator/provider) | Exact end-of-file read; no long AD overflow |
| W008 | Data source with disjoint ZERO ranges | Correct zero semantics and free-space claims |
| W009 | Source symlink, supported length | UDF path-component stream round trip |
| W010 | Owner/mode/creation/access/modify time | Correct on-disk mapping or declared metadata loss |
| W011 | Input outside UDF encoding | Plan rejects or reports permitted explicit loss; never silent truncation |
| W012 | Tiny exact output sizes near first possible volume | Reject impossible geometry; first valid size fits all anchors |
| W013 | Alternative 512/1024/4096 block sizes | Successful external open per independently validated profile |
| W014 | `FSIMG_WRITE_REPRODUCIBLE` | SHA-256 equality across reruns, clocks, process IDs and source scan order |
| W015 | Failure after partial raw sink write | Incomplete output never reported as a published completed image |
| W016 | Reader forced onto generated image | All DATA, ZERO, paths, labels/fields consistent |
| W017 | Opened by NetBSD kernel / 7-Zip | Independent extraction equals source manifest |
| W018 | Passed through NetBSD `fsck_udf -fn` | No structural defects reported by independent checker |
| W019 | 2.50 metadata partition (later profile) | Main/mirror physical disjointness and both read paths |
| W020 | Writer candidate with VAT (later profile) | Map bootstrap and latest-table discoverability |

**No "writer complete" claim** until W001–W018 pass *for the particular writer profile and every advertised block size*. `fsimg` reading its own output is necessary but nowhere near sufficient.

### 12.5 Ruby rule harness integration

Suggested new tests, mirroring the existing FAT spec harness:

```
test/spec/lib/fsimg_spec/udf.rb
test/spec/lib/fsimg_spec/udf/
  bases.rb       # valid byte-level independent bases from documented wire facts
  geometry.rb    # candidate-sector, anchor and VDS range calculations
  model.rb       # typed descriptors, maps and object graph expectations
  builder.rb     # independent builder, NOT using libfsimg or GPL code
  rules.rb       # constraints with legal/illegal ranges and scope
  mutations.rb   # selected dependency-preserving mutations + CRC repair
  cases.rb       # table-driven rule cases
  witnesses.rb   # observable checked failures and minimal positive witnesses
  suite.rb       # runner integration

test/spec/suite/udf/
  test_geometry.rb
  test_rules.rb
  test_builder.rb
  test_model.rb
  test_witnesses_fsimg.rb

test/udf.c
 test/udf-write.c
 test/udf-integration.sh.in
 fuzz/udf.c
```

The actual file placement should follow the present harness conventions; no requirement to introduce a second generator framework. Relevant current sources are [`test/spec/lib/fsimg_spec/fat/`](https://github.com/stass/fsimg/tree/master/test/spec/lib/fsimg_spec/fat), [`test/spec/lib/fsimg_spec/rule.rb`](https://github.com/stass/fsimg/blob/master/test/spec/lib/fsimg_spec/rule.rb), [`test/spec/lib/fsimg_spec/recipe.rb`](https://github.com/stass/fsimg/blob/master/test/spec/lib/fsimg_spec/recipe.rb), [`test/spec/bin/replay`](https://github.com/stass/fsimg/blob/master/test/spec/bin/replay), and [`fuzz/qnx6.c`](https://github.com/stass/fsimg/blob/master/fuzz/qnx6.c).

**Rule example (original illustrative schema, adapt to the actual Ruby Rule API):**

```ruby
# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true

# This is pseudocode describing an assertion, not the current harness API.
# A matching descriptor has a 16-byte tag at its validated location.
rule("udf.tag.checksum", scope: :header) do |descriptor|
  bytes = descriptor.bytes(0, 16)
  expected = bytes.each_with_index.sum { |byte, i| i == 4 ? 0 : byte } & 0xff
  valid_if descriptor.u8(4) == expected
end
```

Because tag checksum and payload CRC are orthogonal, every semantic mutation has two versions: (1) raw bytes modified, checksums left invalid (test generic checksum rejection); (2) descriptor body modified, checksums repaired (test format rule). Where possible, cross-derive expected stage from rule scope, like the existing harness, rather than manually hardcoding expected errors in 200 tests. Shrink until a minimal failing UDF witness remains *valid enough to reach* the intended parser rule.

### 12.6 Runtime, fuzzing, differential and resource gates

* **Quick (`make check`)**: descriptor CRC/tag helper, VRS/probe, empty UDF1.02, one-file/one-directory, inline/short/long descriptors, representative name and intentional corruptions; no external test tools required. Fix small seeds in code or generate them deterministically during test execution.
* **Extended (nightly)**: independent `makefs`, `mkudffs`, `udfinfo`, `fsck_udf`, 7z; all advertised block sizes; >1 million entries/fragmented payload; main/reserve and sparse-hole cases. Gate availability per platform and explicitly distinguish *skip* from *pass*.
* **Deep (opt-in)**: NetBSD and Linux VM read-only mounting, Windows/macOS reader checks, VAT multisession snapshots and BD fixtures when available. Record OS/build/kernel version and filesystem implementation, not just "mount succeeded".
* **libFuzzer**: pure UDF descriptor parser (tag/PVD/LVD/FID/FE/EFE/AD/CS0), whole-image reader (`info`, `verify`, path enumeration), and mapper-specific VAT/sparing/metadata targets. Fuzz within bounded context budgets; inject short reads, cancellation, IO errors and scratch failures. Corpus consists of project-generated lawful fixtures plus targeted mutations, not distributed proprietary disc dumps.
* **Sanitizers**: ASan+UBSan on Linux/macOS/FreeBSD where supported, maximum block size and maximum FID length, plus test builds with tiny `memory_bytes` and scratch to enforce proper `ELIMIT` behavior.
* **Sparse huge volume qualification**: multi-TiB *logical* `fsimg_io_slice`/sparse backing, bounded memory ceiling e.g. 64 MiB, backed by a volume geometry requiring no full-image scan for `info` and demanding scratch-backed ownership for `verify`; actual supported maximum constrained by 32-bit on-disk LBNs and map sizes.
* **Differential**: compare independent reference creator and fsimg writer against `fsck_udf -fn`, two different reader implementations, and a source manifest. When two oracles disagree, investigate against exact ECMA revision; no majority-vote acceptance for invalid layouts.
* **Mutation work bound**: enforce deterministic test timeout/`max_work`/`max_entries` and available memory. A crafted VAT loop should not take hours or thousands of allocations.

### 12.7 Interoperability coverage labels

Use precise claim labels in release notes and `fsimg formats` rather than one broad "UDF supported":

* `udf/physical-1.02:read/verify/write` after all relevant tests pass.
* `udf/physical-1.50-to-2.60:read/verify` only after revision/feature boundary cases have been qualified.
* `udf/vat-1.50:read/verify`, `udf/sparing-1.50:read/verify`, `udf/metadata-2.50:read/verify` independently.
* `udf/metadata-2.50:write` only after native read and `fsck` confirmation; `udf/pow-2.60:write` omitted until actual medium semantics are validated.

An ISO/UDF bridge is a different *view selection* feature and should be advertised and tested separately from UDF's own partition-map and directory features.

## 13. Wire-format offset cribsheet for decoder implementation

These tables restate primitive field **names, widths and locations** in independently authored shorthand; use the normative ECMA document to resolve reserved-byte policy, field semantics and revision-specific additions before coding output. Offsets are within the stated descriptor, start at zero, and all multi-byte numerics are little-endian. They are intentionally implementation guidance, not pasted reference C structs.

### 13.1 Common tag, anchor, AD and identifier

| Object | Offset | Width | Field |
|---|---:|---:|---|
| Descriptor tag | 0 | 2 | TagIdentifier |
| | 2 | 2 | DescriptorVersion |
| | 4 | 1 | TagChecksum |
| | 5 | 1 | Reserved |
| | 6 | 2 | TagSerialNumber |
| | 8 | 2 | DescriptorCRC |
| | 10 | 2 | DescriptorCRCLength |
| | 12 | 4 | TagLocation |
| AVDP | 0 | 16 | Descriptor tag (ID=2) |
| | 16 | 8 | MainVolumeDescriptorSequenceExtent (`extent_ad`) |
| | 24 | 8 | ReserveVolumeDescriptorSequenceExtent |
| | 32 | 480 | Reserved |
| `short_ad` | 0 | 4 | ExtentLength / extent type (top 2 bits) |
| | 4 | 4 | ExtentPosition, relative to implicit partition |
| `long_ad` | 0 | 4 | ExtentLength / type |
| | 4 | 4 | Logical block number |
| | 8 | 2 | Partition reference number |
| | 10 | 6 | Implementation use |
| ICB tag | 0 | 4 | PriorRecordedNumberOfDirectEntries |
| | 4 | 2 | StrategyType |
| | 6 | 2 | StrategyParameter |
| | 8 | 2 | MaximumNumberOfEntries |
| | 10 | 1 | Reserved |
| | 11 | 1 | FileType |
| | 12 | 6 | ParentICBLocation (`lb_addr`) |
| | 18 | 2 | Flags (AD form mask, other strategy flags) |
| `regid` | 0 | 1 | Flags |
| | 1 | 23 | Identifier bytes |
| | 24 | 8 | Identifier suffix bytes |

### 13.2 Core LVD / PD / bitmap

| Descriptor | Offset | Width | Field |
|---|---:|---:|---|
| LVD | 0 | 16 | Descriptor tag ID=6 |
| | 16 | 4 | VolumeDescriptorSequenceNumber |
| | 20 | 64 | DescriptorCharacterSet |
| | 84 | 128 | LogicalVolumeIdentifier |
| | 212 | 4 | LogicalBlockSize |
| | 216 | 32 | DomainIdentifier (`regid`) |
| | 248 | 16 | LogicalVolumeContentsUse (`long_ad` to FSD) |
| | 264 | 4 | MapTableLength |
| | 268 | 4 | NumberOfPartitionMaps |
| | 272 | 32 | ImplementationIdentifier |
| | 304 | 128 | ImplementationUse |
| | 432 | 8 | IntegritySequenceExtent (`extent_ad`) |
| | 440 | variable | PartitionMaps; bounded by MapTableLength |
| PD | 0 | 16 | Descriptor tag ID=5 |
| | 16 | 4 | VolumeDescriptorSequenceNumber |
| | 20 | 2 | PartitionFlags |
| | 22 | 2 | PartitionNumber |
| | 24 | 32 | PartitionContents entity ID |
| | 56 | 128 | PartitionContentsUse |
| | 184 | 4 | AccessType |
| | 188 | 4 | PartitionStartingLocation |
| | 192 | 4 | PartitionLength |
| | 196 | 32 | ImplementationIdentifier |
| | 228 | 128 | ImplementationUse |
| | 356 | 156 | Reserved |
| SBD | 0 | 16 | Descriptor tag ID=264 |
| | 16 | 4 | NumberOfBits |
| | 20 | 4 | NumberOfBytes |
| | 24 | variable | Bitmap; one bit per allocation unit |

The bitmap's declared bits and bytes must be internally consistent; relevant UDF space bitmaps use **1 for free/unallocated** and 0 for allocated/reserved. The tail bits beyond an advertised partition's units must not be counted as usable free units.

### 13.3 FE / EFE / FID exact header offsets

| FE offset | Width | Field |
|---:|---:|---|
| 0 | 16 | Tag ID 261 |
| 16 | 20 | ICBTag |
| 36 | 4 | UID |
| 40 | 4 | GID |
| 44 | 4 | Permissions |
| 48 | 2 | FileLinkCount |
| 50 | 1 | RecordFormat |
| 51 | 1 | RecordDisplayAttributes |
| 52 | 4 | RecordLength |
| 56 | 8 | InformationLength |
| 64 | 8 | LogicalBlocksRecorded |
| 72 | 12 | AccessDateAndTime |
| 84 | 12 | ModificationDateAndTime |
| 96 | 12 | AttributeDateAndTime |
| 108 | 4 | Checkpoint |
| 112 | 16 | ExtendedAttributeICB |
| 128 | 32 | ImplementationIdentifier |
| 160 | 8 | UniqueID |
| 168 | 4 | LengthOfExtendedAttributes |
| 172 | 4 | LengthOfAllocationDescriptors |
| 176 | variable | EAs followed immediately by ADs/inline bytes |

| EFE offset | Width | Field |
|---:|---:|---|
| 0 | 16 | Tag ID 266 |
| 16 | 20 | ICBTag |
| 36 | 4 | UID |
| 40 | 4 | GID |
| 44 | 4 | Permissions |
| 48 | 2 | FileLinkCount |
| 50 | 1 | RecordFormat |
| 51 | 1 | RecordDisplayAttributes |
| 52 | 4 | RecordLength |
| 56 | 8 | InformationLength |
| 64 | 8 | ObjectSize |
| 72 | 8 | LogicalBlocksRecorded |
| 80 | 12 | AccessDateAndTime |
| 92 | 12 | ModificationDateAndTime |
| 104 | 12 | CreationDateAndTime |
| 116 | 12 | AttributeDateAndTime |
| 128 | 4 | Checkpoint |
| 132 | 4 | Reserved |
| 136 | 16 | ExtendedAttributeICB |
| 152 | 16 | StreamDirectoryICB |
| 168 | 32 | ImplementationIdentifier |
| 200 | 8 | UniqueID |
| 208 | 4 | LengthOfExtendedAttributes |
| 212 | 4 | LengthOfAllocationDescriptors |
| 216 | variable | EAs followed by ADs/inline bytes |

**FID header, 38 bytes** (not 36 or 40):

| Offset | Width | Field |
|---:|---:|---|
| 0 | 16 | Tag ID 257 |
| 16 | 2 | FileVersionNumber |
| 18 | 1 | FileCharacteristics |
| 19 | 1 | LengthOfFileIdentifier (byte count) |
| 20 | 16 | ICB target (`long_ad`) |
| 36 | 2 | LengthOfImplementationUse |
| 38 | variable | Implementation use bytes, then CS0 name and 0..3 bytes of padding |

`fid_record_bytes = align_up(38 + imp_use_bytes + name_bytes, 4)`; validate all additions and the enclosing directory's `InformationLength` before advancing. ECMA/UDF parent-entry requirements (including placement in the directory and reserved filename handling) are checked separately. A canonical fsimg writer emits its prescribed parent FID first, then its stable-sorted ordinary children.

### 13.4 Space, time, ownership and name invariants

* A 12-byte timestamp has a type/timezone 16-bit field followed by year `u16`, month/day/hour/minute/second `u8`, centiseconds `u8`, hundreds of microseconds `u8`, and microseconds `u8`. Validate each range and the signed-zone encoding separately; don't convert wall-clock values using host local timezone implicitly.
* File contents encoded by UDF AD type `01` or `10` are **unrecorded** even if disk sectors happen to be physically present or contain nonzero bytes. Never turn them into DATA because the underlying image has bytes.
* For a little-endian bitset, bitmap bit `i` is in byte `i/8`, bit position `i%8`, with bit 1 meaning available. Revision-specific partial-tail/missing-information cases require explicit rules.
* `TagLocation` is **not universally an absolute LBA**; the defining allocation address space matters (volume space vs partition map logical space).
* A UDF file's directory entry and its ICB are not necessarily in one contiguous physical region; FID->ICB always goes through partition translation.
* When computing physical ownership, a repeated FE ICB may represent a legal hard link, but two independent regular files claiming the same data block are not automatically legal.

### 13.5 VAT headers and directory-symbolic-link subrecords

**Virtual Allocation Table version 2.00 and later (logical VAT-file contents):**

| Byte | Size | Field |
|---:|---:|---|
| 0 | 2 | Length of VAT header, normally 152 bytes |
| 2 | 2 | Implementation-use length |
| 4 | 128 | Logical-volume identifier |
| 132 | 4 | Previous VAT ICB location (`0xffffffff` if none) |
| 136 | 4 | Number of files |
| 140 | 4 | Number of directories |
| 144 | 2 | Minimum UDF read revision |
| 146 | 2 | Minimum UDF write revision |
| 148 | 2 | Maximum UDF write revision |
| 150 | 2 | Reserved |
| 152 | variable | Implementation-use bytes; then little-endian 32-bit VAT map entries |

Do **not** assume VAT mapping entries begin at 152 when a nonzero implementation-use area exists; validate and use the header's declared length and implementation-use length for the selected revision. In **UDF 1.50** the old VAT uses a **36-byte trailing record**, rather than the newer 152-byte leading header; the trailing record contains a 32-byte implementation identifier (`*UDF Virtual Alloc Tbl`) and a 4-byte previous-VAT location. The VAT map body precedes that record. These are separate codecs and use revision-dependent checks for the map length, file size and VAT placement. Both versions have the important unmapped-entry value `0xffffffff`.

**Symbolic-link path component record:**

| Byte | Size | Field |
|---:|---:|---|
| 0 | 1 | ComponentType |
| 1 | 1 | LengthOfComponentIdentifier |
| 2 | 2 | ComponentFileVersionNumber |
| 4 | variable | ComponentIdentifier; length from byte 1 |

ComponentType values: `0` reserved, `1` root, `2` mount-root, `3` parent directory, `4` current directory, `5` normal filename. Relative link targets are reconstructed from the sequence, not from path flags in the FE; no dereferencing or host path access is allowed during reconstruction. **Named** components contain a CS0 identifier; root/parent/current components have their own empty-identifier constraints. The record occupies `4 + identifier length` bytes; parsing uses a cursor bounded by the symlink's `InformationLength`.

### 13.6 POSIX permission subset and ICB flag map

UDF FE/EFE 32-bit `Permissions` contains **three independently grouped 5-bit access classes**. Within each group the lowest bits correspond to execute/search, write, read; the remaining bits encode delete and attribute-change permissions which are not expressible in POSIX mode alone. The POSIX-compatible three-bit masks are:

| POSIX entity | UDF permission mask | Conversion to POSIX `rwx` triplet |
|---|---:|---|
| Other | `0x00000007` | Other bits, no shift |
| Group | `0x000000e0` | Shift by 5 to low triplet |
| Owner | `0x00001c00` | Shift by 10 to low triplet |

To construct full POSIX mode, place the three resulting triplets at their usual POSIX shifts: other `<<0`, group `<<3`, owner `<<6`. The extra UDF delete/attribute-change permissions **must be tracked as not preservable by the existing metadata API**, even if `mode` round-trips. The ICB tag flags include `setuid` bit 6, `setgid` bit 7, and sticky bit 8; other ICB flags control file strategy or allocation form and must not be mistaken for mode bits. For exact reading/writing check ECMA-167's permission table and the UDF revision constraints. Test read/write/execute bits separately for all three groups, then separately test special bits and unsupported attribute/delete rights.

## 14. Implementation milestones and completion gates

### Milestone A — codec and recognition

**Deliver:** `lib/formats/udf/{udf.h,codec.c,descriptor.c,volume.c}`, selected `udf` format registration, bounded VRS signature matching, candidate LBS detection, AVDP/VDS validation, PD+Type1 mapping, FSD/root and basic `info`.

**Gate:** generated and independently created UDF1.02 empty images are recognized, fake VRS and corrupt anchors are rejected, no change to FAT/exFAT/QNX6 probe behavior, resources bounded, and `make check`/sanitizer baseline passes. Do not claim `READ` for directories until a usable reader-ops implementation exists.

### Milestone B — physical reader and semantic verification

**Deliver:** FE and EFE, FIDs and Unicode CS0 names, ordinary files/directories, inline/short/long/AED data, symlinks and POSIX metadata, interior ZERO extents, complete structural validation with bitmaps/ownership and optional payload read-through. Implement common extent-data/zero semantics once, and use the same resolver for `cat`, extract, seek and verify.

**Gate:** reader functional R001–R014, R022–R036 applicable to physical volumes; raw/semantic corruptions M001–M030; two independent reference creators (NetBSD makefs, mkudffs + populated mount, or macOS hdiutil) and two independent consumers. Scope only claims versions/physical layouts actually tested.

### Milestone C — baseline deterministic writer

**Deliver:** `lib/formats/udf/{write-plan.c,write-emit.c,write-udf.h}`, UDF 1.02 closed, read-only, Type1 partition writer at selected legal sizes; full deterministic plan, all descriptor checksums, creation label, source metadata-loss policy, bitmap + anchors/VDS redundancy.

**Gate:** W001–W018; native NetBSD checker and at least one kernel reader, stable SHA-256 under reproducible mode, failure/publication semantics, 64 MiB resident budget on synthetic huge sparse images, and no hidden optical mount dependency.

### Milestone D — VAT and sparable readers

**Deliver:** virtual/sparing Type2 map implementation with strictly acyclic bootstrap, logical/physical tag-location separation, last-VAT discovery bounded by sessions, packet remap table, full verify of map and duplicates.

**Gate:** R015–R017, M031–M037, mixed virtual+physical payload, old/new VAT and multiple sparing-copy fixtures, with NetBSD and Linux oracles.

### Milestone E — UDF2.50/2.60 metadata reader

**Deliver:** Metadata File / Mirror / Bitmap partition mapping, physical bootstrap, mirrored read fallback with degraded reporting, EFE streams validation and media-profile negotiation, recognizer for 2.60 POW and realtime special kinds.

**Gate:** R018–R021, M038–M043; corruption of primary and mirror is distinguished, no virtual bootstrap cycle, and `verify` reconciles physical and logical ownership separately.

### Milestone F — newer format writers (independently gated)

**Deliver in separate changes:** 2.01 EFE writer; 2.50 metadata-partition writer; optionally explicit 1.50 VAT/sparing immutable-image writer. Defer 2.60 pseudo-overwrite writing and physical defect sparing unless actual media semantics are in scope.

**Gate:** each newly advertised writer profile has independent full read + `fsck_udf -fn` + OS mount tests, specific geometry/version/media combinations, and no false backwards-compatibility claims.

### Milestone G — scale and qualification

**Deliver:** scratch-backed VAT/AD/ownership indexes, fuzz corpus and exception coverage, multi-million FID traversal, large sparse volume/fragment regression, cross-platform `make check-install`, spec-driven rules with reproducible seed replay and a fixture manifest with oracle versions.

**Gate:** defined memory/scratch/work limits respected even for hostile inputs, no leak or time bomb under sanitizers, and exact capability records match qualification evidence.

### Driver file placement and integration checklist

| Existing location | Expected addition |
|---|---|
| `lib/formats/format.h`, `format.c` | Declare/register `fsimg_udf_format`, no common string special cases |
| `lib/formats/udf/` | Native parser, volume selectors, partition resolver, FID/FE, verifier and writer modules |
| `lib/meta/meta.h`, `extent.c` | Private extent kind for holes (or equivalent private content-region abstraction) |
| `lib/read/reader.c` and `specs/design-content.md` | ZERO delivery, physical eligible-range validation, documented `next_range` policy |
| `include/fsimg.h` | **Prefer no ABI changes** for baseline; only add setting flags if writer needs revision/media selection publicly |
| `test/`, `fuzz/`, `test/spec/` | Separate UDF tests, binary fixtures/builders, fuzz targets and oracle integration |
| `Makefile.am`, `configure.ac` | Driver sources, optional test-oracle discovery, no mandatory runtime deps |
| `specs/design-formats.md`, `specs/README.md` | UDF capability contract and link to a separate driver design |
| `docs/references.md` | All implementation/reference licenses, original standards links, fixture provenance |
| `README.md`, docs/man pages | Exactly advertised UDF read/write, revision/feature limits, CLI profiles and verification limitations |

## 15. Important design choices not to hide behind implementation details

1. **One format identity, multiple revisions.** UDF's VRS/descriptor ecosystem is one format. fsimg's current descriptor provides a *single reader ops table and writer ops table shared by variants*, with capabilities inferred from these tables. Therefore **do not** present a read-only `udf` variant next to a writer-capable `udf102` variant within one unchanged descriptor: the existing registry would mark both WRITE. The simplest compatible path is one `udf` public name, with a writer that initially emits only 1.02, and revision-dependent reader eligibility; later writer-profile settings can be modeled either by explicit validated options or a deliberate extension of per-variant writer capabilities. A collection of separate descriptions must not cause duplicate UDF claims during automatic probe.
2. **UDF data is not synonymous with partition contiguity.** Generalize ZERO segments and physical-eligible ranges in the common content system narrowly, rather than forcing a complicated mount-style API.
3. **Metadata partitions are file-backed.** Their bootstrap depends on reading an ICB through a physical map, not asking an unconstructed metadata map to read itself.
4. **Readers and writers are asymmetrical.** Full 2.60 *read* capability does not imply correctness of a 2.60 *write-once pseudo-overwrite* image; the latter is media semantics and needs a separate acceptance gate.
5. **Full verification needs ownership sets.** Valid tags/CRCs and a successful directory walk do not prove that data blocks are uniquely owned or that bitmap/LVID counts agree.
6. **Immutable image creation is an advantage.** fsimg can build a deterministic graph, allocate once, generate consistent redundant metadata and publish atomically. Don't introduce a UDF journal, packet writer, drive-control API, or mutable in-place update mechanism to solve a problem fsimg does not have.
7. **Compatibility is a matrix, not a boolean.** Report actual UDF version, physical/virtual/sparing/metadata maps, media profile and available read/verify/write operations. Native test evidence determines advertised support.

## 16. Further research and exact-reference map

This file intentionally keeps external source material as **links**, not copied implementations. The following reading order is efficient for an implementer:

1. **Wire decoder:** [ECMA-167](https://ecma-international.org/wp-content/uploads/ECMA-167_3rd_edition_june_1997.pdf) Chapters 3–4, then [NetBSD's BSD-licensed UDF descriptors](https://github.com/NetBSD/src/blob/trunk/sys/fs/udf/ecma167-udf.h) for a second independently licensed cross-check of offsets and descriptor relationships.
2. **UDF revision rules:** [ECMA TR/112-7 (1.02)](https://ecma-international.org/wp-content/uploads/ECMA_TR-112-7_1st_edition_december_2023.pdf), [TR/112-6 (1.50)](https://ecma-international.org/wp-content/uploads/ECMA_TR-112-6_1st_edition_december_2023.pdf), [TR/112-4 (2.01)](https://ecma-international.org/wp-content/uploads/ECMA_TR-112-4_1st_edition_december_2023.pdf), [TR/112-3 (2.50)](https://ecma-international.org/wp-content/uploads/ECMA_TR-112-3_1st_edition_december_2023.pdf), [TR/112-2 (2.60)](https://ecma-international.org/wp-content/uploads/ECMA_TR-112-2_1st_edition_december_2023.pdf).
3. **Volume creation:** [NetBSD `newfs_udf` core](https://github.com/NetBSD/src/blob/trunk/sbin/newfs_udf/udf_core.c), [NetBSD `makefs` UDF populated-tree backend](https://github.com/NetBSD/src/blob/trunk/usr.sbin/makefs/udf.c), and [NetBSD newfs_udf man page](https://man.netbsd.org/newfs_udf.8).
4. **Read/verify:** [NetBSD UDF mapper/allocation](https://github.com/NetBSD/src/blob/trunk/sys/fs/udf/udf_allocation.c), [reader subroutines](https://github.com/NetBSD/src/blob/trunk/sys/fs/udf/udf_subr.c), [NetBSD `fsck_udf` source](https://github.com/NetBSD/src/blob/trunk/sbin/fsck_udf/main.c), and [fsck_udf man page](https://man.netbsd.org/fsck_udf.8).
5. **Cross-platform external oracles:** [udftools](https://github.com/pali/udftools), [mkudffs reference](https://man7.org/linux/man-pages/man8/mkfs.udf.8.html), [7-Zip](https://www.7-zip.org/), [NetBSD makefs(8)](https://man.netbsd.org/makefs.8), [macOS hdiutil(1)](https://www.manpagez.com/man/1/hdiutil/), Linux UDF kernel read-only mount, and Windows native UDF reader.
6. **fsimg integration:** [format descriptor](https://github.com/stass/fsimg/blob/master/lib/formats/format.h), [reader ops](https://github.com/stass/fsimg/blob/master/lib/read/reader.h), [writer ops](https://github.com/stass/fsimg/blob/master/lib/write/writer.h), [extent index](https://github.com/stass/fsimg/blob/master/lib/meta/meta.h), [Ruby spec harness](https://github.com/stass/fsimg/blob/master/specs/design-spec-testing.md), and [security contract](https://github.com/stass/fsimg/blob/master/specs/design-security.md).

**Provenance / usage rule for contributors:** NetBSD, FreeBSD, OpenBSD and other BSD/MIT-licensed files are the preferred reference implementations, but inspect each *actual file's* header and included dependency notices before adapting code; preserve required notices and limit imports to clearly compatible portions. `udftools` (GPL), `libudfread` (LGPL), `libcdio` (GPL) and `udfclient` (Clarified Artistic) are useful black-box validation tools or documentation leads, **not sources of code to copy into BSD-2-Clause fsimg**. The copyrighted ECMA standards are normative **reference documents**, not code to redistribute.
