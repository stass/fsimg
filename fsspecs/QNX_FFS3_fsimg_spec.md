# QNX FFS3 / F3S (EFS) on-flash format — research and fsimg integration specification

**Version:** research draft 1.0  
**Prepared:** 2026-10-20  
**Target:** [`stass/fsimg`](https://github.com/stass/fsimg), BSD-2-Clause C11 library and CLI  
**Proposed public format identifier:** `ffs3` (with `qnx-efs` and `f3s` as possible documentation aliases, **not** additional public names without design review)  
**Status:** detailed implementation input, **not** a complete QNX-certified conformance specification; no implementation was changed or native-QNX tests executed in this research session.  
**Research / code provenance:** original prose, explanatory tables, equations, test plans and algorithms derived from QNX's publicly documented interface behavior and from Apache-2.0/MIT-licensed sources specifically identified in §21. No QNX SDK headers, QNX implementation code, GPL code, or unlicensed implementations are copied or translated here. Specification facts are interoperability observations, not a license to distribute vendor firmware or example disk contents.

> **Critical distinction:** FFS3 is QNX's NOR-flash *Flash FileSystem version 3*, commonly called F3S, EFS or `QSSL_F3S`. It is not QNX4, QNX6 Power-Safe, the QNX IFS boot-image format, ETFS on NAND, or the modern QCFS compressed filesystem. They require separate parsers and writer implementations.

## 0. Reading this specification: confidence and scope

| Evidence grade | Meaning | How implementation should use it |
|---|---|---|
| **D — documented** | Supported by public QNX documentation; typically interface behavior, limits, or tool usage. | A required interoperability target, not necessarily a wire encoding. |
| **S — structured evidence** | Consistent with the Apache-2.0 QNXmount Kaitai description and/or MIT reference parsers and synthetic images. | Implement reader with exact bounds, then qualify on native samples. |
| **O — observed profile** | Seen in specified automotive images, QNX-generated fixtures, or third-party measurements, but not a universal invariant. | May define a narrow profile; do not use as unconditional format law. |
| **U — unverified** | Not resolved to the required confidence, or has conflicting evidence. | Reader recognizes/reports as unsupported when needed; writer cannot advertise support before testing. |
| **P — proposal** | A new fsimg implementation or user-interface decision, not an attribute of the QNX format. | Review alongside existing fsimg architecture before coding. |

**Baseline release goal (P):** read and verify a complete existing FFS3 filesystem in a precisely bounded image/slice, preserving its live namespace and regular-file bytes, POSIX owner/mode, mtime and ctime, little/big endian, reordered logical-to-physical flash units, and safe extraction. Second goal: create **fresh, uncompressed, ECC-disabled**, geometrically explicit FFS3 images that native QNX `devf-ram` mounts and can modify. No in-place filesystem editor, raw NAND/OOB decoder, flash reflasher, automatic forensic recovery, or transparent file decompressor is part of the base driver.

**What this document deliberately will not claim:** an authenticated formal FFS3 wire standard; a fully characterized allocation-status state machine; a verified 255-byte serialized filename; a known meaning for all three status words; an established unit digest algorithm; native writer compatibility for any homemade image; or permission to redistribute QNX binaries or third-party automotive images.

### Evidence anchors

- QNX's [FFS3 system architecture](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/fsys_FFS3.html), [FFS3 user guide](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.user_guide/topic/fsystems_Flash.html), [filesystem feature table](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/fsys_Limitations.html), [flash limits](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.user_guide/topic/limits_Flash.html), and [`mkefs`](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkefs.html).
- Netherlands Forensic Institute's Apache-2.0 [FFS3 Kaitai model](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/qnxmount/efs/parser.ksy) and [filesystem reader](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/qnxmount/efs/interface.py).
- MIT-licensed [qnxprobe FFS3 notes, synthetic fixtures and native comparison](https://github.com/abrignoni/qnxprobe#qnx-efs-in-a-raw-flash-image-either-byte-order-and-deleted-data) and [fixture generator](https://github.com/abrignoni/qnxprobe/blob/dffd27ab242c16100b40114f248e816fb302cc52/tools/make_efs_fixtures.py).
- The actual [fsimg API header](https://github.com/stass/fsimg/blob/master/include/fsimg.h), [format dispatch](https://github.com/stass/fsimg/blob/master/specs/design-formats.md), [metadata model](https://github.com/stass/fsimg/blob/master/specs/design-metadata.md), and [QNX4 driver pattern](https://github.com/stass/fsimg/blob/master/specs/design-qnx4-driver.md).

---

## 1. System model and boundaries

FFS3 is a filesystem designed for **NOR flash**. NOR flash can usually read at byte granularity but program bits only in the allowed direction until an entire erase unit is erased. An erased area is conventionally `0xFF`. FFS3 writes data in **extents**; each extent has a fixed 32-byte metadata record located in a flash unit's header area. Logical files and directory streams consist of chained extents. Replaced or deleted extents remain visible in raw dumps until garbage collection erases their physical unit. The driver supports relocation, wear leveling, garbage collection/reclaim, and power-fail recovery. Native QNX uses a board-specific `devf-*` process for the filesystem *and* flash-device access. [QNX FFS3 architecture](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/fsys_FFS3.html) [D].

A filesystem can occupy only a **slice** of a larger flash socket image. The socket may contain executable boot code, unformatted gaps and more than one FFS3 partition. FFS3 metadata itself supplies physical erase-unit size and a logical-unit map; a generic MBR or GPT need not be present. A filesystem snapshot taken directly from the physical NOR includes stale/deleted data and may have a boot record in a physical erase unit other than unit zero [S/O].

| Name | What it means | Why it matters to fsimg |
|---|---|---|
| Flash socket | Full NOR chip or emulated region, exposed in QNX as a raw `/dev/fsX` resource. | **Not** necessarily one filesystem. |
| Partition / slice | One contiguous FFS3 region of a socket, starting at `partition_base`, of `unit_total * unit_bytes`. | fsimg byte I/O must be relative to its bounded slice. |
| Erase unit, physical unit, block | Smallest independently erasable flash unit; `unit_bytes = 2^unit_pow2`. | This is the right value for fsimg's `allocation_unit_bytes`, not a 512-byte disk sector. |
| Logical unit | FFS3's stable index stored in physical units and named in extent pointers. | Needed for relocation/wear-leveling; *never* equate logical and physical ordinals on an existing image. |
| Spare unit | Physical unit reserved for reclaim/wear-leveling; can have a different recognizable header footprint. | It is unavailable as ordinary payload. |
| Extent | A text payload and associated 32-byte record at the end of its erase unit. | Core unit of file data, directory linking and system metadata. |
| Extent pointer | Pair `(logical_unit, header_index)`. | Not a byte offset; must map via validated logical-to-physical table. |
| Boot record | A small 24-byte FFS3 metadata payload containing `QSSL_F3S`. | Identifies version, geometry, root and original physical placement. |
| `mkefs` | QNX host tool to create embedded flash filesystem images. | Independent writer oracle. |
| `devf-ram` + `flashctl` | QNX in-RAM NOR emulator and management utility. | Independent native mount, modification, remount and re-image oracle. |

QNX 8's feature table states FFS3 supports directories, permissions, symbolic links, modification and status-change times; does **not** support hard links or access times. QNX also states FFS3 has **no literal `.` or `..` directory entries**. [FFS3 user guide](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.user_guide/topic/fsystems_Flash.html) [D]. Those absences must not be mistaken for corruption.

### 1.1 Format/version demarcation

| Format | Typical identifier / location | fsimg response |
|---|---|---|
| FFS3 / F3S / EFS | `QSSL_F3S` in boot-info extent; NOR erase units. | Target `ffs3` driver. |
| QNX6 Power-Safe | Separate superblock, snapshot trees and checksums on block media. | Existing `qnx6` driver. |
| QNX4 | Separate 512-byte block/inode/bitmap scheme. | Existing `qnx4` driver. |
| QNX IFS | Boot image containing OS image directories and ELF startup components. | Separate `qnx-ifs`, *not* FFS3. |
| QNX ETFS | NAND-oriented log/transaction model, often NAND page spare/OOB data. | Separate `etfs`, *not* FFS3. |
| QCFS | Newer QNX compressed filesystem with different on-media format. | Separate `qcfs`, *not* FFS3. |
| Compressed ordinary file on FFS3 | FFS3 file whose bytes happen to carry `deflate`/`inflator` or other application wrappers. | Return raw file content first; optional decoding is a separate content filter with explicit identification. |

QNX historically described transparent flash decompression; QNX 8 `devf-ram` documentation explicitly says FFS3 no longer has **built-in** decompression, directing users to `deflate`/`inflator`. Do not conflate an Audi compressed file or `iwlyfmbp` signature with an FFS3 on-disk compression bit. [QNX 8 devf-ram](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/d/devf-ram.html) [D]; [MMI3G F3S notes](https://github.com/dspl1236/MMI3G-Toolkit/blob/e9ca3469835e3e1c78dabfb5cbe5676d2b752500/research/F3S_FORMAT.md) [O].

## 2. Public compatibility and operational limits

| Property | Published QNX constraint | Proposed initial fsimg handling |
|---|---|---|
| Filename length | Up to **255 bytes**, not Unicode characters. | Reader accepts validated on-disk names; writer's exact upper bound is **gated** by 8-bit `namelen` including the observed terminator. Initially cap conservatively at 254 name bytes until 255 is tested. |
| Path length | **1024 bytes** excluding mountpoint and NUL. | Validate **encoded** bytes at every step; source/host path limitations may be narrower. |
| File size | **2^31 - 1 bytes** documented. | Enforce documented ceiling; additionally check actual extent count and 16-bit reference capacities. |
| Filesystem / disk size | QNX documentation gives **2^31 - 1 bytes**. | Reject creation beyond this published interoperability profile; reader of larger images is unqualified, not a license to assume overflow. |
| Directory size | Limited by available media space. | Derived from number and lengths of directory-entry extents; no `.` or `..` slots. |
| Access time | Not recorded/updated. | Do not advertise `FSIMG_META_ACCESSED`. |
| Change time | Supported; 32-bit field observed. | `FSIMG_META_CHANGED`, seconds, UTC; no nanosecond precision. |
| Modification time | Supported; 32-bit field observed. | `FSIMG_META_MODIFIED`, seconds, UTC. |
| Creation/birth time | No separate field in baseline 20-byte stat record. | Do not advertise `FSIMG_META_CREATED` merely because the field is named `ctime`. |
| Hard links | Unsupported by FFS3. | Do not synthesize multi-name inode sharing; reject required hard-link preservation. |
| Symlinks | Supported. | Decode content as link target; filesystem paths never traverse it while extracting. |
| Special nodes | Native runtime may expose FIFOs, character/block/sockets; exact representation needs fixtures. | Reader can recognize kinds; unsupported file types return `FSIMG_EUNSUPPORTED` under existing fsimg closed model. |
| Timezone | QNX `mkefs mtime` input is interpreted as UTC. | `FSIMG_TIME_UTC` with one-second precision for supported times. |
| ECC | `mkefs ecc_on=0,1,2` controls off / 64-byte / 32-byte-aligned ECC. | Base writer supports **ECC off only**; ECC variants recognized but require independent checks. |

QNX source: [flash limits](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.user_guide/topic/limits_Flash.html), [FFS3 feature table](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/fsys_Limitations.html), [mkefs](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkefs.html). **Conflict to document, not smooth over:** `mkefs` describes a *default `max_size` option of 4 GB*, while published FFS3 filesystem/file limits are 2 GB minus one byte. An option's default is not evidence that a 4 GB FFS3 volume is mountable. The 2 GB limit is the conservative fsimg profile pending a native large-volume test.

**Physical geometry (D/S):** QNX's `mkefs block_size` default is **64 KiB** and depends on actual flash devices/interleaving. Native examples use 64 KiB, 128 KiB, 256 KiB and 512 KiB units, whereas parser-only synthetic fixtures may use **4 KiB** units to exercise pointer decoding. Do not reject a well-formed readable image merely for failing a proposed writer's chosen native geometry. `spare_blocks` defaults to one; reserve at least one if the filesystem must reclaim space after native modifications.

---

## 3. Binary serialization model

All scalar fields below are **unsigned integers** unless specified. Each physical image declares its on-media byte order with the `endian` character in the first system extent: ASCII `L` for little endian, `B` for big endian [S/O]. The whole partition uses that order for its multi-byte metadata; **do not** apply the host endian and **do not** assume LE globally as some Kaitai reference code does.

Define these quantities:

- `B` = physical partition base byte offset within surrounding input. The fsimg `fsimg_io_t` representing a slice makes `B=0` to the driver.
- `U` = erase-unit size = `1 << unit_pow2`.
- `N` = `unit_total`, total physical erase units in this FFS3 partition.
- `R` = `unit_spare`, number of reserved/spare units.
- `A` = text alignment = `1 << align_pow2`, normally 4 or 64 in observed profiles.
- `H` = 32, serialized extent-header size.
- `p` = physical unit ordinal, `0 <= p < N`.
- `k` = an extent-header index within one physical unit, starting at 0.

Calculations (all overflow-checked before pointer arithmetic):

```text
volume_size       = N * U
unit_base(p)      = B + p * U
header_at(p,k)    = unit_base(p) + U - 32 * (k + 1)
encoded_text_pos  = (text_offset_hi << 16) | text_offset_lo
text_at(p,k)      = unit_base(p) + (encoded_text_pos << align_pow2)
text_size(p,k)    = header.text_size
```

This makes explicit a defining property of FFS3: **data texts grow into an erase unit from its low-address end; extent headers are stored from the high-address end backward.** An extent's payload must be inside its own unit and not overlap *any* live or retired extent-header slots. A file's content is not a single sector run inferred from file size; it is the concatenation of its validated data extent payloads.

### 3.1 Normal initial image (illustrative, not fixed placement)

A common freshly built little-endian image with `A=4` begins its first useful logical unit as follows [S/O]:

```text
physical unit start +0x0000    unit-info payload (16 bytes; extent header #0)
                    +0x0010    logical-unit payload (24 bytes; header #1)
                    +0x0028    boot-info payload (24 bytes; header #2)
                    +0x0040    root-directory payload (variable; header #3)
                    +...       directory entries, file texts, alignment gaps
unit end - 0x0080              extent-header #3 (32 bytes)
unit end - 0x0060              extent-header #2 (32 bytes)
unit end - 0x0040              extent-header #1 (32 bytes)
unit end - 0x0020              extent-header #0 (32 bytes)
```

At this layout the 8-byte signature begins at **unit-relative 0x2C**, *not* byte 0. This is just one normal geometry. With `A=64` some automotive samples place it at **0xC4**, and reclaim/wear-leveling can move the boot-bearing *physical unit* to any legal ordinal. Therefore: **no universal fixed absolute magic offset** [S/O]. Verify the boot pointer and current unit layout instead of identifying images using a sole byte scan for `QSSL_F3S`.

## 4. Physical erase-unit information record (16 bytes)

This is a payload record usually described by header index 0, not a freestanding sector header.

| Offset | Bytes | Meaning | Evidence / validation |
|---:|---:|---|---|
| 0 | 2 | `struct_size` | `16` in established profile; reject incompatible structure lengths until qualified. |
| 2 | 1 | `endian` | ASCII `L` or `B`; must match the decoder's scalar order. |
| 3 | 1 | padding | Observed erased `0xFF`; treat variants conservatively. |
| 4 | 2 | `unit_pow2` | `U = 2^unit_pow2`, power-of-two erase unit. |
| 6 | 2 | reserved | Common value `0xFFFF`. |
| 8 | 4 | `erase_count` | Physical wear / erase-count information; not a logical generation. |
| 12 | 2 | boot extent logical unit | First half of `(logical,index)` pointer. |
| 14 | 2 | boot extent index | Second half of boot pointer. |

**Parser requirements:** never blindly dereference the boot pointer before the logical-unit index exists. Bootstrap unit size from a plausible unit-info payload, then independently read and verify its extent header #0 from the unit's tail; confirm header's claimed text matches the payload. Check `unit_pow2` for safe shifts and the slice can hold at least one whole unit. A clone of the first 16 bytes without a valid backing header is not enough to recognize an FFS3 partition. Multiple physical units should agree on byte order and `unit_pow2`; differing erase counts are normal.

Some SPARE states retain unit info but no valid ordinary unit-logical payload; this is an intentional class, not necessarily damaged data. The sequence of normal and spare physical units requires a state-aware parser, not an assumption that every `unit_info` is followed by an allocated logical unit.

## 5. Logical-unit identity record (typically 24 bytes)

Stored as extent header #1's text on a normal allocated logical unit [S].

| Offset | Bytes | Meaning | Confidence |
|---:|---:|---|---|
| 0 | 2 | `struct_size` | Usually 24; extended records may exist. [S] |
| 2 | 2 | `logical_unit` | Stable logical unit ID. [S] |
| 4 | 4 | `age` | Unit age / wear-level state. [S]; exact ordering rules [U]. |
| 8 | 16 | additional 128-bit bytes | Named `md5` in one permissive parser; **algorithm/coverage/purpose not independently established** [U]. |

If the record length is extended, do not assume the 16-byte tail remains at offset 8. Retain opaque extension bytes or reject unsupported versions as appropriate. **Never** fill a digest based on an invented MD5 formula or reject a fixture because an unvalidated digest happens to be zero.

For opening an existing image construct a physical mapping:

```text
physical p = 0..N-1
    parse this unit's own system extents (or recognized spare footprint)
    if allocated ordinary unit: obtain logical ID l
    require 1 <= l <= N - R   [within the established active profile]
    associate l -> p, rejecting duplicate live ownership

root_and_file_pointers -> logical unit l -> physical unit p -> extent header k
```

**Important gate:** the `1..N-R` range and uniqueness hold for the observed normal / synthetic profile; some partial-recovery or transitional real-world images may show stale physical-unit identities. The native startup driver's rules for choosing the active of two conflicting logical units are [U]. Do not resolve duplicates by “last one wins,” erase count, `age`, or physical order without empirical proof; distinguish `FSIMG_EUNSUPPORTED` (known recovery state) from `FSIMG_ECORRUPT` (invalid structure).

## 6. Partition boot information record (24 bytes)

Typically the text of **system extent #2** in the physical unit that hosts logical unit 1. All offsets here are relative to the beginning of boot-info text, **not** the partition.

| Offset | Bytes | Field | Description / checks |
|---:|---:|---|---|
| 0 | 2 | `struct_size` | `24` in qualified profile. |
| 2 | 1 | `rev_major` | `3` for FFS3. |
| 3 | 1 | `rev_minor` | `0` in reference fixtures; other versions are gated. |
| 4 | 8 | signature | ASCII `QSSL_F3S` (8 bytes, **no** trailing NUL in this field). |
| 12 | 2 | `unit_index` | Physical ordinal of unit containing this boot record. |
| 14 | 2 | `unit_total` | Number of physical units `N`. |
| 16 | 2 | `unit_spare` | Number of reserved/spare units `R`. |
| 18 | 2 | `align_pow2` | File/metadata extent text offset quantization exponent. |
| 20 | 2 | `root.logical_unit` | Root directory extent pointer logical unit. |
| 22 | 2 | `root.header_index` | Root directory extent pointer index. |

Require `unit_total > 0`, `0 <= unit_index < unit_total`, `unit_spare < unit_total`, a sane `align_pow2`, `U % A == 0`, `N*U` within the slice and the published compatibility ceiling (for writer). Reject impossible pointers, future major versions and known unsupported formats explicitly. `root` is an actual pointer; default `(1,3)` is **not** an immutable on-disk requirement. If a file embeds `QSSL_F3S` by accident, it is not a second boot record.

For a candidate boot signature at outer byte offset `S`, a discovery routine should identify the containing erase-unit **start** `P` by validating a candidate unit-info/footer pairing, **not** by simply masking `S` to a page boundary. With `P` verified, the enclosing partition begins at:

```text
B = P - boot.unit_index * U
```

Then confirm `B` is nonnegative, correctly aligned for the medium, `B + N*U <= image_size`, the candidate belongs to the boot-info extent pointed at by system metadata, and the root can be read. This supports boot relocation and multi-partition flash dumps [S/O]. The start of an arbitrary slice is supplied externally by `--offset`/`--length` or a higher-level partition wrapper; do not silently scan beyond the slice boundary.

---
## 7. Extent header (32 bytes): central FFS3 record

The per-extent record is **exactly 32 bytes in the observed FFS3 v3 profile**. Header `k` is at `unit_end - 32*(k+1)`. Two different extents can share a logical unit but have different `k` indices; a pointer identifies *an extent header*, not a data byte or an inode.

### 7.1 Field layout

| Relative offset | Width | Field | Interpretation |
|---:|---:|---|---|
| `0x00` | 4 | `status[0]` | State, lifecycle flags, extent type. Decode with partition endian. |
| `0x04` | 4 | `status[1]` | Redundant/alternate status field; full semantics **unknown**. |
| `0x08` | 4 | `status[2]` | Redundant/alternate status field; full semantics **unknown**. |
| `0x0C` | 6 | `ecc[6]` | ECC bytes, or erased placeholders on non-ECC media; algorithm **not qualified**. |
| `0x12` | 1 | `reserve` | Usually `0xFF`. |
| `0x13` | 1 | `text_offset_hi` | Upper 8 bits of quantized 24-bit relative offset. |
| `0x14` | 2 | `text_offset_lo` | Lower 16 bits of quantized 24-bit relative offset, media endian. |
| `0x16` | 2 | `text_size` | Payload byte count, `0..65535`; not necessarily a full allocation cluster. |
| `0x18` | 2 | `next.logical_unit` | Next extent of same chain, when the relevant status flag permits. |
| `0x1A` | 2 | `next.header_index` | Next extent's index. |
| `0x1C` | 2 | `super.logical_unit` | Replacement/superseding extent, when present. |
| `0x1E` | 2 | `super.header_index` | Replacement/superseding header index. |

Every referenced text interval must satisfy `text_offset <= U`, `text_size <= U - text_offset`, alignment and metadata-space bounds. A positive extent text size cannot pass into the next unit; there is no implied cross-unit continuation. Endianness applies to `text_offset_lo`, not the single upper byte. A 24-bit encoded offset is not 24-bit byte addressing until multiplied by `A`.

**Examples:** with `A=4`, high part `0`, low part `0x0010` denotes offset `0x40`; with `A=64`, that same encoded field denotes offset `0x400`. Do not read the 3-byte offset as an ordinary `u24` in native host endian without explicitly reconstructing its field order.

### 7.2 Status[0] bits

The public, permissively licensed Kaitai model names the following bit fields [S]. All masks are expressed on the decoded 32-bit integer.

| Mask | Bit(s) | Name | Safe reader interpretation |
|---:|---:|---|---|
| `0x00000001` | 0 | `NO_WRITE` | A state flag. Detailed behavior under partial power-fail writes is [U]. |
| `0x00000002` | 1 | `NO_NEXT` | 1: no next chain pointer; 0: use `next`. |
| `0x00000004` | 2 | `NO_SUPER` | 1: not superseded; 0: follow `super` to newer extent. |
| `0x00000008` | 3 | `NO_SPLIT` | Split/fragment lifecycle indicator; exact operational semantics [U]. |
| `0x00000070` | 4–6 | `CONDITION` | Three-bit lifecycle state; see below. |
| `0x00000080` | 7 | `EXT_LAST` | Last recognized header / table boundary. Beware all-`FF` erased terminal slots. |
| `0x00000300` | 8–9 | `TYPE` | 1: system, 2: directory record, 3: file data in observed profile. 0: other/unallocated, handle statefully. |
| `0x00000400` | 10 | `BASIC` | Basic extent marker. Meaning beyond nomenclature [U]. |
| `0xFFFFF800` | 11–31 | reserved/state | Common freshly built header has ones in reserved bits; do not invent semantics. |

Observed condition **bit patterns**, not a complete FFS3 state-machine contract:

| Encoded bits in `status[0]` | Observed interpretation | Writer policy |
|---|---|---|
| `0x30` (three-bit condition=3) | Allocated/in-use extent in QNXprobe-generated fixtures. | Use **only after** native `mkefs` and `devf-ram` comparison. |
| `0x10` (condition=1) | Deleted extent in synthetic and forensically analyzed images. | Do not create deleted extents in a fresh fsimg writer. |
| `0xFFFFFFFF` full erased header | Erased/unwritten footer. | Terminal sentinel, not a file extent or always corruption. |
| Others | May encode interrupted writes, recoverable states, reclaim stages. | Preserve evidence; fail unsupported for live traversal when unqualified. |

A synthetic writer that wants an allocated `TYPE=FILE` extent frequently generates `0xFFFFF800 | BASIC | 0x300 | 0x30 | NO_SPLIT | NO_NEXT | NO_SUPER`. This line documents an **observed mask combination**, **not an endorsed final writer rule**. `EXT_LAST` may be signaled via an erased next header rather than a dedicated allocated terminal record. Do not assume the three status words are equal or act like a quorum checksum; full recovery semantics await native mutation/power-fail samples.

### 7.3 Extent kinds and pointer semantics

| Header `TYPE` | Value bits | Typical text | Next chain means |
|---|---:|---|---|
| Unallocated / other | `0x000` | Erased space or transition record | Never treat an unknown state as live data. |
| System | `0x100` | `unit_info`, `unit_logi`, `boot_info`, etc. | Internal metadata, profile-specific. |
| Directory | `0x200` | A variable-length directory entry, including a root or child. | Next sibling directory entry in parent directory stream. |
| File | `0x300` | Regular file data or symlink target bytes. | Next consecutive segment of the same file/target. |

The `TYPE` header alone cannot tell `regular file` versus `symlink` or `FIFO`; the **directory entry's POSIX mode** identifies the user-visible kind. System metadata is *not* a user file named `.logi`. Root is a directory-entry payload referenced by `boot_info.root`.

A canonical sentinel pointer in the permissive generated fixtures is `(0xFFFF,0xFFFF)`, while `NO_NEXT` / `NO_SUPER` flags declare whether a pointer is meaningful. The flags take priority; an unused pair need not be dereferenced. A live pointer with `NO_NEXT=0` or `NO_SUPER=0` must address an in-range mapped logical unit and existing header. Empty directory/file record corner cases must be tested before requiring an allocated empty-data extent universally.

### 7.4 Parsing an erase unit without inventing an extent count

The flash unit stores no ordinary forward array of headers. Decode from the unit's tail, each record at an increasing index. The last-marker semantics are unusual because an untouched/erased 32-byte footer has its highest-state bits set. A robust parser has to distinguish: (1) a well-formed allocated or deleted extent; (2) an ordinary erased terminal slot; (3) a genuine stateful last marker; and (4) an invalid half-programmed footer. For the baseline reader [P]:

1. Bounds-check `k` before computing `32*(k+1)` and ensure a header cannot overlap previously discovered text.
2. Decode all 32 bytes, retaining all 3 status words and ECC raw bytes for later verification, even if only status[0] is acted upon.
3. Treat a fully erased footer as an end-of-list, not as a meaningful allocated extent.
4. For a non-erased footer, check supported type/condition combinations and its text interval.
5. Track every header/text range; stop on a supported `EXT_LAST` pattern or an erased terminal footer.
6. Impose a hard scan cap no larger than `U/32`, the caller's work budget, and any earlier text/header collision bound.
7. A physical unit judged spare may have only unit-info and different status[1] characteristics: branch by *validated unit state* rather than applying ordinary-unit invariants blindly.

Do not accept text bytes by scanning for patterns that look like `stat_s`. Such matching can create false filenames inside compressed binaries or inside deleted/unreachable space.

## 8. Directory entries: hierarchy and POSIX stat payload

An allocated directory-record extent (`TYPE=DIR`) stores a **variable-length serialized dirent**. At its beginning is an 8-byte header, then a padded name, then an observed 20-byte stat record. The dirent's `first` pointer has object-type-dependent meaning: for a child *directory*, its first child directory-entry extent; for a regular file or symlink, its first data extent.

### 8.1 Directory-entry header (8 bytes)

| Offset | Bytes | Field | Meaning |
|---:|---:|---|---|
| 0 | 2 | `struct_size` | Normally `8`, size of fixed directory-entry header only. |
| 2 | 1 | `moves` | Move/rename activity counter [S]; semantics and wrap rules [U]. |
| 3 | 1 | `namelen` | Encoded byte length of name field in observed v3 layout. |
| 4 | 2 | `first.logical_unit` | First content extent or directory child-head logical unit. |
| 6 | 2 | `first.header_index` | Index of first content/child extent. |
| 8 | variable | `name` | Observed NUL-terminated encoded bytes, then padding to multiple of 4 in common profile. |
| after padded name | 20+ | `stat` | POSIX mode/owner/timestamps. |

For the observed 8+name+20 profile:

```text
name_field_length = namelen          # in QNXmount/fixture profile, includes NUL
name_storage      = round_up(name_field_length, 4)
stat_at           = dirent_start + 8 + name_storage
minimum_bytes     = 8 + name_storage + 20
```

**Gate on name semantics:** QNX publicly allows a 255-*byte name*, while observed `namelen` is 8-bit and includes the NUL terminator (synthetic/native-reference parser assumptions). A literal 255-name-byte entry would require 256 bytes including NUL, not representable in 8 bits. The most likely possibilities include a different encoding rule for maximum-length names or an actual narrower accepted bound in this version. Do not falsify this by claiming full 255-byte binary support. Investigate real `mkefs` names of 254/255 bytes, inspect the stored `namelen` and reference `devf-ram` behavior. Until then advertise the conservative writable maximum of **254 bytes** (or narrower if fsimg's UTF-8 representation requires it).

**Name interpretation:** treat directory entry name as a sequence of bounded bytes. The permissive Kaitai parser uses UTF-8, but that alone does not establish all QNX filesystems enforce UTF-8. For fsimg's C NUL-terminated path API, invalid UTF-8 or embedded NUL needs explicit unsupported/corrupt handling, never lossful replacement or path aliasing. Reject names containing `/`, prohibited `.` and `..` pseudoentries in the namespace, zero-length child names, and lengths beyond supported on-disk capacity. Root may have an empty name, or mount-name semantics that should **not** be exposed as an extra directory level. Consult `mount` from `mkefs` before choosing how to report the root entry.

### 8.2 Stat record (20 bytes)

| Offset relative to stat | Width | Field | Use in fsimg |
|---:|---:|---|---|
| 0 | 2 | `struct_size` | `20` established profile. |
| 2 | 2 | `mode` | POSIX `S_IFMT` type plus low 12 permission/special bits. |
| 4 | 4 | `uid` | Numeric user identity; `FSIMG_META_OWNER`. |
| 8 | 4 | `gid` | Numeric group identity; same `FSIMG_META_OWNER`. |
| 12 | 4 | `mtime` | Unsigned seconds since Unix epoch, UTC; `FSIMG_META_MODIFIED`. |
| 16 | 4 | `ctime` | Unsigned status/metadata change time, UTC; `FSIMG_META_CHANGED`. |

`ctime` **does not mean filesystem object birth time**. There is no `atime` in the record. A nonzero `mtime` copied from a QNX source is not a proof of when the file was created. Interpret and report time as seconds with 1-second stored precision. The signedness and post-2038 native treatment of 32-bit timestamps should be tested; the baseline codec can preserve their raw `u32` bit pattern and report dates within the supported range, but must not silently rebase time zones.

POSIX file kind comes from `mode & 0170000` (`S_IFMT`); representative values:

| File kind | Mode type bits | fsimg mapping |
|---|---:|---|
| Regular | `0100000` (`0x8000`) | `FSIMG_ENTRY_FILE` |
| Directory | `0040000` (`0x4000`) | `FSIMG_ENTRY_DIRECTORY` |
| Symbolic link | `0120000` (`0xA000`) | `FSIMG_ENTRY_SYMLINK` |
| FIFO | `0010000` (`0x1000`) | `FSIMG_ENTRY_FIFO` when native zero-data semantics are verified |
| Character device | `0020000` (`0x2000`) | Recognize and decline under current fsimg closed entry kinds |
| Block device | `0060000` (`0x6000`) | Recognize and decline under current fsimg closed entry kinds |
| Socket | `0140000` (`0xC000`) | Recognize and decline under current fsimg closed entry kinds |

Direct creation by QNX `mkefs` buildfiles lists `file`, `dir`, `link`; the **mounted runtime** can create FIFOs as shown by the Netherlands Forensic Institute's native `devf-ram` fixture script. This proves **read** coverage is worth implementing, not that the fresh-image writer already knows a universally mountable FIFO encoding.

### 8.3 Root, directories, and sibling chains

A root directory entry is reached by `boot.root`. Each directory entry's `first` points to the **first child directory-entry extent**, if any. Following that child extent's **`next` pointer** walks the siblings in that directory. Each child with type directory recursively points to its own children. There are no synthesized `.` or `..` payload entries. The parent-child relation is established *by traversal*, not stored as a parent inode number. [NFI interface](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/qnxmount/efs/interface.py) [S].

```text
boot.root -> ROOT dirent
              first -> A dirent --next--> B dirent --next--> C dirent
                       first -> data ...     first -> children ...
                                   ^ one or more next-linked file extents
```

When an extent is superseded, *first follow its `super` chain to the effective version*, then parse that version's directory metadata / successor fields. Never emit obsolete dirents merely because they occur in the same physical unit. The filesystem has no global flat inode table or central pathname index. The live namespace comes from reachability from the root; directory-record extents that exist elsewhere are historical/orphaned data.

### 8.4 File payload, multi-extent files, symlinks and empty files

For a regular file, start with its dirent `first` pointer; resolve any `super` replacements; read validated payload bytes and follow its `next` pointer until the terminal flag. Concatenate the texts in that *logical chain order*, not physical offset order. A file's logical length is the sum of payload byte counts in the final resolved chain; the 20-byte `stat` does not carry a file size. Empty files may use an empty content extent or special sentinel convention: **native oracle gate**.

For symbolic links, the same storage mechanism holds the link-target bytes; caller returns `FSIMG_ENTRY_SYMLINK` and the link target is its logical file content, without dereferencing it. Whether native links carry or omit a terminating NUL in the content requires byte-level qualification, although synthetic qnxprobe fixtures store targets as raw bytes and the fsimg API requires no terminating NUL. Do not interpret a link target as a pathname to traverse while creating or extracting image trees.

For FIFO/special nodes, `mode` governs type. QNX FFS3 is not a general-purpose device-node serialization API in fsimg's initial scope. The read driver should preserve size=0 for confirmed FIFOs and flag other types explicitly, without reading random allocated text as a FIFO payload.

**Content range model (fsimg):** an FFS3 file's confirmed extents are logically fully initialized (`initialized == logical`). If an extent chain is broken, do **not** silently inject zeros or return a truncated file as success. `fsimg_reader_read_at()` and `fsimg_reader_next_range()` must agree on the exact same resolved extent map, and a short read may return confirmed bytes **with an error**, as allowed by fsimg's common content contract. [fsimg content design](https://github.com/stass/fsimg/blob/master/specs/design-content.md).

---

## 9. Recoverable stale/deleted data vs current filesystem

FFS3's on-flash record system is designed so a previous version can survive until erasure. The `super` (superseding extent) pointer can lead from an old extent to its replacement. Directory entries and ordinary file-data extents are both eligible for this lifecycle. A deleted file may be named by an obsolete dirent or only partly represented by orphaned data extents; later allocations and reclaim can destroy any historical connection.

**For the ordinary fsimg reader:** the only visible namespace is the root-reachable, current set. A deleted file is **not** an extra ordinary file in the directory. A superseded extent must not be appended to the new data. Pointer loops, broken live chains or inconsistent active state fail closed.

**Possible later forensic extension (P):** a distinct read-only recovered-items enumerator (not normal `fsimg_reader_next`), reporting provenance and recoverability. The MIT-licensed qnxprobe research usefully distinguishes:

1. Deleted entries whose full current extent chain is still present.
2. Deleted entries with broken/missing/reused references (known name, incomplete payload).
3. Orphaned runs of stale/superseded extents whose parent directory entry is gone or ambiguous.

For each recovery item record location, original extent pointer(s), deleted/allocated/superseded state, byte spans, whether the returned bytes form a **complete** file, whether the name is certain, and whether the parent path is proven from a reachable chain. Never claim a single canonical historical parent, original size or original file version when these cannot be established. `qnxprobe` reports thousands of recoverable/deleted extents in six automotive NOR images and acknowledges some contents cannot be reproduced. Those numbers are **reported empirical results**, not format guarantees. [qnxprobe EFS section](https://github.com/abrignoni/qnxprobe#qnx-efs-in-a-raw-flash-image-either-byte-order-and-deleted-data) [O].

### 9.1 Power-failure/recovery caveat

QNX FFS3 documents atomic rename, fault recovery and reclaim under native drivers. **Those guarantees belong to the implementation's mutation protocol**; merely writing syntactically recognizable `super` and status bits is not a proof of power-fail safety. fsimg is not implementing a mutable flash driver; do not implement speculative runtime rewriting of old extent headers, garbage collection or erase-count balancing in the fresh image builder. For read-only damaged snapshots, either reproduce the native driver's active-view decisions where demonstrated or report that recovery state as unsupported without guessing.

## 10. Space accounting and consistency

Keep distinct accounts for:

- `image_bytes = N * U` and `spare_bytes = R * U` (spares **not** usable file payload).
- System extent payloads and all extent headers.
- Directory-entry payloads (including root and any retained historical entries).
- Current file payload bytes, retired payload bytes and alignment gaps.
- Truly erased/unused bytes and work space needed for native reclaim.

A reader with a fully identified image can classify physical byte ranges into `header`, `text`, `padding`, `erased`, and `unidentified`. Validate no reported non-erased interval is double-owned by two incompatible headers. For a normal clean image, erased slack consists of `0xFF` bytes. For a damaged/truncated dump the checker must distinguish missing source bytes from erased bytes. Optionally report the qnxprobe-style *unaccounted non-`0xFF` bytes* metric as a diagnostic, but don't assert zero for all possible historical/partially recovered media.

The published QNX [`flashctl -i`](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/f/flashctl.html) reports `Total`, `Spare`, `Headers`, `Padding`, `Overhead`, `Free`, `Stale`, `Avail.`, `Reserved`, and per-unit erase statistics. These numbers are a strong native comparison oracle; fsimg should explain any discrepancy rather than turning `stale` into `free` on a fresh image. Because reclaim changes the physical availability state, an input reader may withhold `FSIMG_VOLUME_FREE` until its free-space derivation is native-qualified.

---
## 11. fsimg architecture and API integration

This is **not** a stand-alone FUSE implementation. It must reuse fsimg's existing descriptors, bounded I/O, catalog, prepared-directory traversal, external scratch, source identity checks, entry-kind and metadata policy, safe extraction, and staged output publication. The current project explicitly avoids an in-place editing VFS. [fsimg principles](https://github.com/stass/fsimg/blob/master/specs/principles.md), [architecture](https://github.com/stass/fsimg/blob/master/specs/architecture.md), [API](https://github.com/stass/fsimg/blob/master/specs/design-api.md).

### 11.1 Proposed module structure (P)

```text
fsspecs/QNX_FFS3_fsimg_spec.md        # this research document, when checked in
specs/design-ffs3.md                 # future authoritative format design
specs/design-ffs3-driver.md          # future authoritative driver/qualification contract
lib/formats/ffs3/
    descriptor.c                     # public name, probe and capability records
    ffs3.h                           # private decoded types, limits, invariants
    codec.c                          # endian/record decode+encode, no packed host structs
    probe.c                          # bounded identity; optional raw socket scan path
    units.c                          # physical/logical map, spare states, boot discovery
    extents.c                        # header iteration, pointer resolver, cycle checks
    directory.c                      # live entry parsing, sibling and child chains
    read-ffs3.c                      # reader ops, per-entry content index
    verify.c                         # geometry, graph and physical accounting
    write-ffs3.c                     # guarded native-qualified writer (phase 2)
    layout.c                         # immutable allocation plan / metadata spool
    emit.c                           # FF-fill, metadata and payload layout
lib/formats/formats.c                # add one descriptor once ready
fuzz/ffs3.c                          # corpus-guided reader/codec fuzz targets
test/ffs3.c                          # structural/unit tests and reader fixtures
test/data/ffs3/                     # independent small synthetic fixtures and hashes
```

This is illustrative; match actual registry path discovered in tree rather than adding a second registry. Avoid general shared "flash framework" abstractions until at least another format demonstrably reuses them. The only generic changes justified by FFS3 should be small and independently tested.

### 11.2 Public capability record (P)

`fsimg_format_info_t` from `include/fsimg.h` already provides byte-order settings and fields for file/volume maxima, kinds and metadata. It is almost sufficient for FFS3, except for erased-byte output and optional spare/ECC settings.

| Capability field | Proposed `ffs3` | Qualification |
|---|---|---|
| `.name` | `ffs3` | No `qnx6` collision. |
| `.profile` | `NOR flash v3, uncompressed` or variant-specific concise label | Do not advertise ECC and compressed profiles before testing. |
| `.entry_kinds` | Reader `FILE|DIRECTORY|SYMLINK`; add `FIFO` after native read fixture passes. | Writer may offer fewer kinds independently. |
| `.metadata_fields` | `MODIFIED|CHANGED|OWNER|MODE` | No CREATED, ACCESSED, filesystem generic DOS attributes. |
| `.allocation_unit_size_mask` | Power-of-two supported **erase units** (e.g. 64K/128K/256K/512K), optionally 4K parser-only fixtures. | Reader capability separate from writer's legal geometries. |
| `.write_allocation_unit_size_mask` | Initially only native-tested 64K and 128K; expand on oracle success. | Do not mark 4K test fixtures as native flash-usable. |
| `.sector_size_mask` | If current API insists on a disk-sector field, present 512 as **logical transport convention** and document that FFS3 has no 512-byte physical sector. | Prefer a semantic `erase-unit` geometry label in detailed reporting. |
| `.max_file_bytes` | `0x7fffffff` | QNX documented limit; actual image capacity can be lower. |
| `.max_volume_bytes` | `0x7fffffff` | Conservative QNX compatibility limit. |
| `.max_name_utf16_units` | Conservative conversion bound for valid UTF-8, subject to verified name-byte cap; **not** a UTF-16 on-media limit. | API mismatch must be documented; consider a future generic `.max_name_bytes` capability for byte-limited POSIX filesystems rather than misrepresenting the actual storage unit. |
| `.max_label_bytes` | `0` | Root mount path is **not** a FAT-style volume label. |
| `.write_settings` | `FSIMG_SETTING_BYTE_ORDER` plus proposed spare/alignment configuration bits. | ECC unsupported initially. |
| `.reader/.writer` | Independent ops pointers; writer absent or unregistered until native acceptance. | Follow existing fsimg unqualified-writer descriptor procedure. |

Do not use the `FSIMG_VOLUME_UUID` or volume serial fields for FFS3 without finding an actual corresponding on-media field. The 16-byte `unit_logi` opaque value has no demonstrated global-volume UUID meaning. A stored mountpoint is a QNX runtime path, **not** an fsimg `label`; the first writer profile can use a fixed/native default mountpoint until a dedicated capability is reviewed.

### 11.3 Discovery and the existing bounded-probe conflict (P)

**This is a necessary design decision, not an optional performance optimization.** fsimg's existing [`lib/formats/format.h`](https://github.com/stass/fsimg/blob/master/lib/formats/format.h) defines a probe as a pure function of a **bounded prefix** and total slice length; auto-detection will not scan arbitrarily deep. On FFS3, the only strong identity string, `QSSL_F3S`, is itself stored in a **movable physical unit**. Real NOR acquisitions have boot records located beyond the first unit. A fixed prefix of a few kilobytes cannot be both complete and cheap for this format.

**Two-tier proposal** preserving other formats' semantics:

1. **Normal auto-probe (fast):** if the first part of the slice contains a boot-record candidate, require the associated matching physical unit-info + extent header + boot-info agreement; `FSIMG_OK` only after its identity is established. A weak `unit_info` alone is not a unique signature. Otherwise return `FSIMG_EFORMAT` (not claimed), with no false positive.
2. **Forced `-F ffs3` / `read_options.format="ffs3"`:** add a narrowly scoped *format-owned deep-identify callback* that reads, under explicit work and byte budgets, more of the supplied **bounded slice**, looking for a qualified boot record / mapped unit. This operation must be validation, not a bypass of the normal probe contract. If insufficient budget or missing required format features, return `FSIMG_ELIMIT` or `FSIMG_EUNSUPPORTED` with exact reason. Do not silently claim success on a bare magic match.
3. **Raw socket discovery:** an explicit top-level operation (future `fsimg --scan-volumes` / library iterator) finds **multiple candidate FFS3 partition slices** using the `unit_index` geometry equation in §6. It must not alter standard single-volume `reader_open`, and must deduplicate distinct signatures belonging to the same partition, reject overlap/ambiguity, and respect `--offset`/`--length` and resource budgets. A raw flash image containing two FFS3 partitions is not itself a single FFS3 volume.

**Alternative if no generic deep-identify API is approved:** restrict automatic and forced recognition to FFS3 volumes with an early validated boot record, *document the limitation prominently*, and make users carve/move an image to a supported layout for analysis. **Do not** weaken signature requirements to accept any 16-byte `unit_info` preamble as FFS3. This would miss a proven real-world case, hence the first proposal is preferred.

### 11.4 Erased-flash fill: essential generic emission extension (P)

**This is the most consequential integration issue discovered in fsimg.** [`specs/design-emission.md`](https://github.com/stass/fsimg/blob/master/specs/design-emission.md) currently **initializes every destination byte to `0x00`** before calling a driver, and its generic content copier skips explicit ZERO ranges on the assumption that the whole destination is already zero. A flash image must represent **erased** unused space with `0xFF`. FFS3 therefore cannot be implemented correctly by adding an ordinary writer descriptor alone.

Required small, principled change:

- Give the format emission plan a **destination initial fill byte**, default `0x00` for every existing format and `0xFF` for FFS3. Initialize the whole new image to that byte, through the common bounded adapter; preserve pre-mutation alias/durability checks and failure rules.
- Provide a common `fill_range(offset,length,byte)`/equivalent **windowed** operation, or extend existing initialization with a bounded fill generator; do not expose raw sink access to the driver. Filling a 2 GiB raw image with `0xFF` is physically O(image size) and cannot be represented by a regular sparse zero-hole without a distinct storage transform.
- **Crucial:** for `initial_fill=0xFF`, a source whose initialized bytes are **zero** must emit actual `0x00` bytes to its planned data extents. The current rule “ZERO source copies nothing” would otherwise produce a file full of `0xFF` instead of zero. Differentiate `unused flash` (erased FF) from `initialized file contents` (arbitrary bytes including zero) in the common copy helper.
- Permit source copy to overlay bounded planned content spans while leaving inter-extent *unallocated* alignment padding at `0xFF`. If an extent advertises allocated length beyond logical initialized bytes, its content bytes must be deterministic and valid; do not accidentally return padding FF as user data.
- Keep the existing zero-fill fast path exactly unchanged for FAT/exFAT/QNX4/QNX6 and disk-container sparse output. A compressed/container layer should see actual logical `0xFF` bytes and must not classify them as all-zero holes.

Acceptance tests for this extension should include: all-zero regular file, zero-source provider, empty file, `0xFF`-valued source file, mixed sparse source with long zero range, padding boundaries, fresh file outputs and reused/nonempty callback sinks. A zero-source file and an erased extent are fundamentally different FFS3 states.

### 11.5 Typed format-specific write settings (P)

Existing `fsimg_write_options_t` has `allocation_unit_bytes`, `byte_order`, `volume_bytes`, `min_free_bytes`, `epoch`, and `flags` (including `FSIMG_WRITE_REPRODUCIBLE`). It lacks **explicit spare erase-unit count** and **text alignment/ECC mode**. An FFS3 writer should not smuggle these settings through existing unrelated `reserve_percent`, `inode_capacity`, label or UUID fields.

Recommended minimal public settings (ABI work reviewed under existing options version rules):

| Setting | CLI proposal | API representation | Initial behavior |
|---|---|---|---|
| Erase unit | `-o cluster=131072` (existing generic alias; optionally `-o ffs3:erase_unit=131072`) | `allocation_unit_bytes` | Default 65536 bytes; validated powers of two. |
| Byte order | `-o endian=little|big` | Existing `byte_order` | LE default; qualify BE write natively. |
| Spare units | `-o ffs3:spare_units=1` | New field / tagged format-option or one new setting bit | Default 1, checked `1 <= R < N` for modifiable images. |
| Alignment | `-o ffs3:align=4` | New field / setting bit | 4 bytes for ECC off in baseline; reserved 32/64 for future ECC profiles. |
| ECC | `-o ffs3:ecc=none` | Optional future enum, not a serialized unsupported flag | Any non-`none` value rejected until ECC codec and native validation exist. |
| Mountpoint | `-o ffs3:mount=/flash` | Future bounded string field | Postpone/custom profile until root/mount encoding qualified; no misuse of label. |
| Free space | `--free-space=BYTES` | Existing `min_free_bytes` | Required data-usable free bytes **after** spare units and structural overhead. |
| Reproducibility | `-R -e epoch` | Existing flags and epoch | Deterministic tree ordering, timestamps and erased fill. |

`-o` currently accepts typed layer settings; add these specifically to FFS3's key set and its capability reporting, not as global accepted arguments on FAT/QNX6. If project policy forbids expanding public `fsimg_write_options_t` before next ABI version, start a **one-spare, four-byte-aligned profile** with fixed values and expose additional settings only in a designed future version; never claim arbitrary configurable spares with no API. `-L` default must be suppressed or rejected for formats advertising zero label capacity, because the root flash mountpoint is not a volume label.

### 11.6 Reader operations and snapshot lifetime

Adopt the same lifecycle as `lib/formats/qnx4/` and `lib/formats/qnx6/`:

- `probe`: read no more than declared or opt-in deep budget and perform no filesystem traversal.
- `open`: validate main geometry, fixed system records, logical map and boot/root identity; do not allocate per-file payloads.
- `next`: hand common traversal a prepared directory of named dirent handles, with validated unique path components and per-entry metadata. Return depth-first in on-media chain order through common traversal rules.
- `read_at` / `next_range`: provide bounded logical→physical data mapping for the **current** regular file or symlink target, lazily enough not to allocate a whole file's extents when that violates budgets.
- `verify`: header-only, structural graph or payload scopes, consistent with `FSIMG_VALIDATION_HEADER`, `STRUCTURAL`, `PAYLOAD`; no return of `complete=true` if any included scope is unexamined.
- `free`: release the same session, scratch pages, and cache through the correct owner.

A header-only open may not inspect all historical extents, but it **must** reject an invalid root/boot or impossible geometry. `FSIMG_CAP_WRITE` stays false while the writer is unqualified; add the reader to `--formats` only when its public contract passes mandatory reader acceptance.

### 11.7 Expected CLI behavior (existing syntax + proposed settings)

**Read/list/info/verify/extract on a known FFS3 volume (after driver exists):**

```sh
fsimg i  -f efs.img
fsimg t  -f efs.img -F ffs3
fsimg W  -f efs.img -F ffs3
fsimg x  -f efs.img -F ffs3 -C ./output
fsimg x  -O -f efs.img -F ffs3 path/to/file > recovered.bin
fsimg --formats --json
```

For an FFS3 *slice within a larger image*, the **actual existing fsimg options** are `--offset` and `--length` (not `-b` or `-z`; `-z` means compression in fsimg):

```sh
fsimg t -f raw-nor.bin --offset=33554432 --length=8388608 -F ffs3
```

**Fresh image creation examples, pending writer qualification:**

```sh
fsimg cf fresh.efs -F ffs3 -o cluster=131072 -o endian=little \
    -o ffs3:spare_units=1 -R -C ./root .
fsimg cWf fresh.efs -F ffs3 -s 8388608 -o cluster=65536 \
    -o ffs3:spare_units=1 -R -C ./root .
```

The `ffs3:` settings above are **proposed interface design**, not supported by today's fsimg binary. Existing create syntax should be checked against actual implemented parser before using the examples as a smoke test. Avoid implying the FFS3 driver already exists.

---

## 12. Reader: concrete algorithms, error modes and bounds

### 12.1 Boot discovery / partition opening

**Input:** a bounded fsimg I/O slice, optionally an exact requested FFS3 format. **Output:** validated boot record, endian, geometry, physical-unit mapping and root pointer. Algorithm outline written for this design:

1. Establish input byte length. Refuse empty or shorter-than-bootstrap data.
2. In fast mode, look for a *candidate* signature in the permitted prefix, but do not accept it without its struct-size, version, unit-info/extent-header and slice-consistency evidence.
3. In explicit scan mode, seek candidate signatures inside scan budgets and derive candidate unit boundaries from plausible unit-power geometries. Derive partition base from boot `unit_index` **only after verifying the candidate's enclosing physical unit**.
4. For each candidate, select endian from the physical unit's unit-info (`L`/`B`), re-decode entire record set in that endian and check the `QSSL_F3S` boot record's exact 8 bytes and size/version.
5. Bound `N*U` against the supplied region, `R < N`, text alignment, and the required bounded list of physical units.
6. Read each unit's own text/footer (or validated spare footprint) to establish a physical-to-logical map and verify the declared spare count to the extent possible.
7. Confirm boot logical-unit pointer resolves to the same boot data and that `root` resolves to an allocated/appropriate directory extent.
8. Record the format profile (LE/BE, unit size, alignment, observed ECC evidence, status unknowns) and publish a supported reader only if the core invariants hold.

**Resource controls:** use `fsimg_io_read_at()` and common resource accounting exclusively; no entire volume mmap, eager `read(image)` or recursive external Python process. Max input bytes, max units, max header inspections, max candidate signatures, max work, and cancellation all apply. Require overflow checks for shifts, base correction and every range even when `unit_pow2`, `unit_total` and `align_pow2` came from seemingly valid records.

### 12.2 Extent-pointer resolution

An extent reference is a pair `(l,k)`. A decoder should first demand `l` maps to one **validated live physical unit**, `k` is an in-range header index, header `k` exists in that unit, and its type/state is consistent with the caller. If the extent is marked superseded (`NO_SUPER` cleared), follow its superseder pointer, using a bounded cycle detector. Repeat until a current version is reached. Do not consider an unallocated extent “success” because its bytes happen to contain plausible text. Historical/forensic resolution has a different entrypoint and rules.

A bounded Floyd-style cycle detector can detect pathological `super` or `next` loops in constant memory, while a scratch-backed ownership ledger provides exact duplicate/shared-chain detection. A Bloom filter is not sufficient for correctness. Be careful not to reject valid cases in which an overwritten dirent remains reachable through an earlier chain link; the **effective** extents are what define the visible stream.

### 12.3 Directory enumeration and content

For each directory to prepare:

1. Resolve the directory's current dirent, validate its mode as directory and obtain its `first` child pointer or independently qualified empty-directory representation.
2. Follow child `TYPE=DIR` chain, resolving superseded child dirents on each step. Track both raw-pointer and effective-pointer cycles; cap by available real extent headers, not by user-supplied file sizes.
3. Parse the fixed eight-byte header and length-delimited name, check NUL/padding/record length, parse 20-byte stat and validate POSIX type bits.
4. Validate child names and duplicate/case rules with common prepared-directory code. FFS3 does not have FAT-style case-folding or DOS 8.3 aliases; use exact byte/UTF-8 comparison under chosen supported profile. A conflicting exact name in the **active** directory is corruption unless a proven supersede rule resolves it.
5. For a regular/symlink child, resolve its file extent chain on demand and compute logical size with checked addition; expose content range pieces in chain order. For a child directory, prepare its own contents only when traversal descends.
6. Return the current entry's UID/GID/mode/mtime/ctime exactly as represented; report unsupported types explicitly, not as empty files.

**Directory integrity:** Reject a directory referring to itself as its own descendant, two unrelated parents sharing the same directory entry, chain cross-links that create aliases fsimg can't model, and names that escape the extraction root. Do not follow symlinks while resolving children. A damaged current graph is corruption even if a stale previous version of the bytes could be recovered separately.

### 12.4 Range reads and streaming

Represent a file by an ordered list of *logical offsets* mapping to validated absolute image byte ranges: `(logical_base, physical_text_offset, text_size)`. Build incrementally with optional spill to scratch, or resolve a small window lazily from its validated chain under configured work caps. The expected file length is the sum of extents. Binary-search or page the range index for random `read_at` without rescanning the whole file on every request. The `next_range` interface returns physically backed DATA ranges for the current file and no invented ZERO regions; any zeros returned to the caller must be the file's actual stored zero bytes.

For a very large number of tiny extents, bound RAM by the per-file index/page cache and scratch; total work may be proportional to extent count but no `O(file_size)` payload allocation is allowed. Cache ownership must be session-specific to avoid leaking information between two users/images.

### 12.5 Verification scopes and failure taxonomy

| Scope | Actions | On failure |
|---|---|---|
| **HEADER** | Bound slice; verify unit geometry, byte order, boot identity, format revision and root pointer existence. | `FSIMG_EFORMAT` if no identity; `ECORRUPT` if recognized but internally impossible; `EUNSUPPORTED` if recognized unknown revision/ECC profile. |
| **STRUCTURAL** | Traverse live tree; inspect every reachable extent header, logical map, file chains, duplicates/cycles and all names/metadata; optional physical accounting for every unit under work budget. | Inconsistent references `ECORRUPT`, unimplemented valid state `EUNSUPPORTED`, budget `ELIMIT`. |
| **PAYLOAD** | In addition to structural checks, read every present file byte through same range operations and prove no unexpected short reads; optional per-file manifest/hash validation when supplied. | I/O/truncation/cancellation as appropriate. No invented file checksum unless format actually stores it. |

Never treat a signature-only match as `FSIMG_CAP_VERIFY` success. When a field is unknown but the supported live view remains valid (e.g. opaque `unit_logi` 16 bytes, unqualified status[1]/[2]), note it in detailed info/diagnostics rather than inventing a mismatch. Reserve strict unknown-state failure for facts that could change current live content or correctness.

---
## 13. Fresh FFS3 writer design (phase 2, native-qualified only)

### 13.1 Supported vs deferred

**Initial qualified profile proposed:** static fresh image, ECC disabled, byte-order LE (BE only after a BE native mount), 64 or 128 KiB physical erase units, 4-byte alignment, at least one spare unit, ordinary POSIX regular files and directories, symlinks, UID/GID/mode and mtime/ctime. Data-only image output, no on-flash edits. Do not claim FIFO write support until a native-built FIFO's header/directory payloads can be produced and mounted. No embedded compression, bad-sector table, wear-level replay, encrypted payload or in-place patching.

**What should not be shipped as an FFS3 writer:** a packed list of directory headers and file bodies following `QSSL_F3S` without the **32-byte extent headers at the tail of each unit**, valid pointer chains, per-unit logical metadata, spare unit states and required status/ECC fields. The MIT Rust prototype described in §20 is useful for experimentation but its inspected `build()` path populates the header/text region without demonstrating the requisite tail extent-table serialization or native mounting. It is **not an interchangeable oracle or validated FFS3 emitter**.

### 13.2 Immutable planning model

The existing fsimg writer makes one plan before changing the sink. Follow that pattern:

1. **Collect and validate** sources through common fsimg source scanner/catalog; reject hard-link semantics, external special nodes, invalid paths, unsupported timestamps or oversized files. Determine effective defaults from options, with a reproducible epoch.
2. **Choose volume geometry:** `U`, `N`, `R`, `A`, byte order, root mount policy, and maximum file extent payload `E`. Recommend `E=4096` as an easy native-runtime style first profile; separately test `E=16384` to align with QNX `mkefs` creation behavior. Both values are below the header's 65535-byte representation. Use only profile combinations the native driver mounts.
3. **Reserve unit infrastructure:** each allocated physical unit's `unit_info` and `unit_logi`; selected boot-bearing unit's boot-info; root dirent; sufficient physical spare unit(s), each with the appropriate observed native metadata. Place first boot in logical unit 1 for a fresh image if native comparisons demonstrate this as the common valid profile; no need to emulate wear-level relocation while creating from scratch.
4. **Lay out all directory entries** in a stable order based on fsimg prepared catalog, with parent first-pointer and sibling next-pointer relationships. Directory entries are separate directory-type extents. A parent dirent must be allocated before its child pointer is finalized but no in-place write is needed during planning.
5. **Split file content** into bounded data extents, recording actual payload byte count on each. A regular file may need many extent headers and can cross erase units. A symlink target gets an appropriate data chain. Empty-file encodings require native qualification, not default fallbacks.
6. **Pack physical units** with two growing regions: aligned text allocations from the low end and 32-byte headers reserved from the high end. No text, padding and header may overlap. Leave enough room for unit-specific system metadata and any required blank terminator/header; do not fill the last byte then discover no room for the header.
7. **Assign stable `(logical_unit,index)` pointers** only after allocating all extents. Resolve all `next` and `first` links and the boot/root pointer. Opaque digest/ECC fields and status words must come from a validated native-compatible rule, not placeholders chosen for appearance.
8. **Prove the plan:** every byte range accounted for, every encoded pointer resolvable, no cycles/shared data, `total = N*U`, `R >= 1`, valid `text_size`, `N` within 16-bit representability, total length within the supported upper bound, and `free_bytes >= min_free_bytes` after spare and overhead reservations.
9. **Emit once** using shared `0xFF` initialization, metadata window writes and the existing source-copy operation with explicit zero-content materialization. Complete all source identity checks, sink alias checks and durability requirements before first output mutation.
10. **Verify staged output** using the newly implemented reader and optional `fsimg cW`, then **independently** mount in native `devf-ram` as a release gate; self-read alone is not proof of format correctness.

### 13.3 Placement equation and feasibility tests

For each physical unit maintain a planned `front` cursor (next byte for aligned text), a count `h` of reserved extent headers, and any mandatory terminal header space `T` (native-gated). To place a text of `L` bytes:

```text
next_text = round_up(front, A)
new_front = next_text + L
new_h     = h + 1
require new_front <= U - 32*new_h - T
```

Do not incorrectly count text padding as content. Split an object only when the next payload plus its own header cannot fit; if zero payload needs a header, handle it explicitly. Pack logical filesystem units before assigning physical ordinals so that the first image uses a simple identity mapping; permit the reader to handle complex remappings and moved boot units. Reserve physical spares separately; their serialized `unit_info` and special header must match a QNX-formatted blank unit, not an arbitrary all-FF block unless the native oracle establishes that.

For a layout with `N` units, at least `R` are spare. A crude **upper bound** on eligible content bytes before metadata is `(N-R)*U`, not `N*U`. Real usable space is lower due to per-unit system metadata, 32-byte headers, alignment, root and dirent records, and native reserved reclaim budgets. If a user supplies an exact image size, reject as too small instead of silently deleting files or reducing spare count. If automatic sizing is requested, use fsimg's existing first-fit / minimization contract over legal whole-unit increments and record which geometry the planner chose.

### 13.4 Emission details

After common destination fill with `0xFF`, writer generates fixed-size metadata windows and calls the common metadata-write helper at each planned target offset. Source payload copying refers to immutable catalog IDs and explicit scatter ranges in the order that file bytes are logically read; no arbitrary temporary file holding all file data. Distinguish two fill cases:

- *Unallocated space, inter-record gaps, unused unit tails*: `0xFF`.
- *Allocated file content whose source supplies ZERO*: `0x00` for the full extent text count, including all-zero files; not skipped.

The encoding of integer fields depends only on selected target endian and tested profile, not host architecture. An FFS3-specific version of the common emitter must not open host files on its own, bypass source-stability checks, or write directly to a caller-owned file handle. If source content changes during emission, mark the staged output incomplete per fsimg contract.

**Important physical caveat:** the fsimg result is a byte image, not a live flash-programming command stream. A valid image can be copied to an erased NOR simulator in a QNX VM. Real hardware reflashing, ECC/OOB layout, protection sectors and power-fail-safe updates are separate device- and firmware-specific operations and are not a claimed feature.

### 13.5 Status words, boot placement and digest gate before enabling create

A general-purpose builder could emit plausible `unit_info`, `boot_info`, `dirent` and `stat` fields yet fail native mounting because flash drivers also interpret lifecycle flags, redundant status words and padding. Prior to promoting a writer from private experimental descriptor to public `.writer`:

- Diff **native `mkefs`** freshly created images at the record-field level, at 64K and 128K, LE and BE, with one/two spare units and multiple data extents.
- Compare generated `unit_info`, `unit_logi`, `boot_info`, system/data/dirent header ordering, status[0..2], padding, `ecc[6]` when off, spare state and blank terminator rules.
- Mount our image on `devf-ram`, list and hash every file, check POSIX metadata, mutate a file, `sync`, unmount/restart/reopen and re-read. A successful `qnxmount` read is **not** enough.
- Verify that the native driver does not immediately classify our supposedly free sectors as reclaim candidates or corrupt headers; inspect `flashctl -i` accounting, including `Stale`, `Spare`, `Headers` and `Reserved`.
- Test an empty FS, empty directory, empty file, symlink, maximum name, system mountpoint and optional FIFO independently.

### 13.6 Geometry, alignment and ECC profiles

| Profile | Unit size | Alignment | ECC | Native write target |
|---|---|---:|---|---|
| Common default | 64 KiB | 4 B (`align_pow2=2`) | Off | Preferred first release. |
| Native fixture | 128 KiB | 4 B | Off | Primary second release gate; NFI sample uses this. |
| Automotive profile | 256 KiB | 4 B | Off | Qualify read first; writer when tested. |
| Bigger native RAM profile | 512 KiB | 4 B | Off | Read/writer gated by native profile. |
| ECC mode 2 | device-dependent | 32 B (`align_pow2=5`) | 32-byte ECC | **Reader detect/report, writer unsupported** until algorithm/driver verified. |
| ECC mode 1 | device-dependent | 64 B (`align_pow2=6`) | 64-byte ECC | **Reader detect/report, writer unsupported** until algorithm/driver verified. |
| Synthetic unit stress | 4 KiB | 4 B | Off | Unit/reader fuzz fixture **only**, not advertised as hardware mountable. |

QNX [`mkefs ecc_on`](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkefs.html) documents 0=off, 1=64-byte alignment, 2=32-byte alignment; [`flashctl -b`](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/f/flashctl.html) uses alignment *exponent* (`-b6` and `-b5`). An Audi file with `align_pow2=6` is **not proof** that its payload is compressed or that every 64-byte-aligned image has an ECC algorithm fsimg can reproduce. Verify independently.

---

## 14. Safety, malformed images, and threat model

An FFS3 volume can be supplied by an untrusted image obtained from a damaged or malicious device. The reader must not depend on a mounted filesystem's recovery code to validate an image. Mandatory cases:

| Input anomaly | Required response |
|---|---|
| Fake `QSSL_F3S` inside a regular file, deleted data or random flash | No claimed partition without a structurally valid owning boot extent and unit-info/geometry. |
| Magic in first 512 bytes but impossible `unit_index` | Recognized/corrupt if identity proven; never wrap signed subtraction to find a huge base. |
| `unit_pow2` or `align_pow2` outside safe arithmetic | Fail before shift; no undefined behavior. |
| `N*U` or `p*U` overflow | Fail before seek/allocation. |
| Boot record extends beyond physical unit / slice | Corrupt/truncated rather than tolerant partial parse. |
| `R >= N`, missing or duplicate logical mapping | Fail or explicit unsupported recovery state; no `last wins`. |
| Live pointer to spare/missing unit, header outside unit | Corrupt. |
| `text_size` extends into header area or adjacent erase unit | Corrupt. |
| `text_offset_hi` large enough to overflow shift | Corrupt before offset arithmetic. |
| `next` self-cycle, supersede cycle, two-parent directory cycle | Corrupt; bound visited nodes and work. |
| `stat_s` size 0 or smaller than defined record | Corrupt; never cast arbitrary memory to host struct. |
| `namelen` 0 for non-root, missing NUL, embedded separator/NUL, invalid padded size | Corrupt or unrepresentable name under explicit profile; never escape extraction root. |
| Duplicated active name with incompatible contents | Corrupt unless a proven supersede selection resolves it. |
| Unknown lifecycle condition affecting a live file | `EUNSUPPORTED`/`ECORRUPT` as supported by evidence; never emit stale bytes silently. |
| Huge number of zero-length extents | Work/extent count budget; prevent endless traversal. |
| Non-UTF-8 name under strict UTF-8 API | Explicit unsupported/profile limitation; no replacement that merges different names. |
| Huge source image with a trillion decoy magic candidates | Scan-byte, candidate-count and time/work budgets, cancellation and explicit user opt-in. |
| File names `..`, absolute host paths or symlinks aimed outside extraction root | Common fsimg safe extractor rejects/normalizes according to its published contract; never follow media symlinks. |
| Metadata claiming device node/socket outside closed fsimg entry kinds | Recognition plus explicit unsupported output, no device creation on host. |
| Truncated source or short read | Precise short/I/O error with confirmed byte count; never zero-pad and declare success. |
| File extent shared by multiple unrelated files | Investigate if hard links; FFS3 prohibits them; reject if no proven valid explanation. |
| ECC-enabled image | Report profile; don't pretend 6 raw bytes are a validated ECC code. |

**Memory and work policy:** all lists, physical maps, extent state tables and per-directory namespace preparation must obey independently configured budgets. Use paged arrays or fsimg scratch when enumeration exceeds memory limits. Keep recursive-tree traversal iterative as fsimg already specifies, with a bounded ancestor graph/cycle detector. No `assert` as a substitute for validation in release builds. No unchecked compression processing: FFS3 file payload extraction is raw and does not execute embedded data.

**Physical provenance:** forensics read should distinguish exact image offsets, logical unit numbers, base/slice offsets, and whether an object is live, deleted, superseded or orphaned. An untrusted legacy parser's guessed file length is not authoritative. For a partial dump, do not claim a complete filesystem or make reconstructed images silently writable on real flash hardware.

## 15. Implementation phases and acceptance criteria

### Phase A — research fixtures, codecs and report-only prototype

- Add hand-written endian-aware codecs for the 16/24/24/32/8/20-byte v3 structures, with exhaustive field/range unit tests and both endian fixtures.
- Build/read the physical/logical map and report boot pointer, erased-unit size, `N`, `R`, `A`, root pointer, header count, status distributions and physical positions.
- Compare binary decodes with qnxmount and qnxprobe on the licensed-permissive test fixture sets; at this stage no fsimg reader advertised.
- Characterize native `mkefs` output with 64/128 KiB units, empty files, symlinks, 254/255-byte names, 4KiB/16KiB data extents, 1/2 spares, LE/BE and mountpoint variations; record results in `docs/ffs3-research.md`.

**Exit A:** no unchecked arithmetic, field-level fixture agreement and a completed uncertainty log.

### Phase B — read-only integration + structural verification

- Implement format descriptor and bounded normal probe; decide opt-in deep discovery semantics in authoritative `specs/design-ffs3-driver.md`.
- Add boot/physical unit map, system-header decoding, live directory graph and file data `read_at`/`next_range`, with independent content reads through qnxprobe or qnxmount.
- Support POSIX regular/dir/link, owner/mode/mtime/ctime, and FIFO read after native fixture. Reuse existing extraction worker, avoid creating special files on host.
- Implement structural verifier, corrupt-header fuzz and resource-limit/host-path tests. Verify extents resolve through superseding chains on native overwritten-file image, and boot-bearing unit can move to nonzero physical position.
- Define explicit “recognized but unsupported” diagnostics for unknown lifecycle/ECC situations; no silent fallback to a heuristic stream scan.

**Exit B:** all qnxmount native fixtures and independent LE/BE synthetic corpus round-trip; `fsimg t`, `x`, `-O`, `W`, `i` correct with bounded memory and fuzz safety. Writer stays unpublished.

### Phase C — enable erased-byte emission and fresh writer privately

- Extend common writer initialization from hardcoded zero to format-selected erased fill byte; extend zero-source copy as in §11.4.
- Implement fresh extent planner and little-endian ECC-off 64KiB and 128KiB variants, initially behind a private unregistered writer descriptor as QNX6/QNX4 qualification precedent requires.
- Check all bytes deterministically; `fsimg` self-verify after staging, independent qnxmount/qnxprobe extract, **then** native QNX `devf-ram` mount with files and modifications.
- Compare `flashctl -i` accounting before/after first runtime write and multiple remounts; fix discrepancies and encode lessons in authoritative format design rather than in undocumented constants.

**Exit C:** native QNX mounts, reads, modifies, syncs and remounts every qualified profile; reproducible bytes and metadata are deterministic; no writing to actual devices.

### Phase D — expand qualified profiles, optional raw socket discovery

- Big-endian writer, 256KiB units, multiple-spare configurations, 255-byte name boundary when established, FIFO write if native verified.
- Explicit `--scan-volumes` for multiple NOR FFS3 partitions, resilience to relocated boot and decoy magic; add impact/cost limits. Separate optional forensic salvage from ordinary live namespace.
- ECC, compression, unknown revisions and partially interrupted writes remain **explicitly unqualified** until independent formats/oracles become available. Do not advertise them based on plausible field names.

**Exit D:** complete release manifest with exact supported geometries and known rejections, no claims of universal on-flash recovery.

---
## 16. Testing tools and reproducible native-QNX workflows

FFS3 should be tested against **both** an independent *reader* and an independent *writer/runtime*. Read-only Python parsers that share a Kaitai description are useful for cross-checking offsets but are not substitutes for the authoritative `mkefs` + native `devf-ram` driver pair.

### 16.1 Tool matrix (read / write / modify / verdict)

| Tool | Platform / license | Identify/read/list/extract | Create fresh FFS3 | Modify mounted FFS3 | Why it matters |
|---|---|---|---|---|---|
| **QNX `mkefs` / `mkxfs -t ffs3`** | QNX SDP host utility; proprietary, **license required** | Not a generic reader | **Yes** | No | **Primary native independent writer oracle.** Produces buildfile-controlled v3 flash images. |
| **QNX `devf-ram`** | QNX target utility; proprietary | **Yes**, through native filesystem mount | Via `flashctl` formatting | **Yes**, normal POSIX I/O | **Primary native independent runtime oracle.** Simulates NOR flash in RAM. |
| **QNX `flashctl`** | QNX target utility; proprietary | Partition info, erase/mount, accounting via `-i` | **Format/erase** emulated flash (different from mkefs tree image) | Via mounted FS, not generic image edit | Check spares, headers, reclaim, geometry and mount acceptance. |
| **QNX POSIX `find`, `ls`, `stat` if installed, `dd`, `cp -V`, `tar`, `cksum`, `cmp`** | QNX target user utilities | Yes | Filesystem operations via devf | **Yes** | Independent namespace/content/metadata oracle; availability varies by QNX version. |
| **`qnxmount` EFS** | Linux, Python/Kaitai/FUSE; **Apache-2.0** | **Yes**, read-only mount and direct Python API | No | No | High-value format model and QNX-generated fixture/tar. Assumes little-endian; separate source license notice. |
| **`qnxprobe`** | Cross-platform Python stdlib; **MIT** | **Yes**, LE/BE, multiple raw NOR slices, deleted data | **Synthetic fixture generator only** | No | Strong independent cross-check of current tree and raw physical allocations. |
| **TRACE Forensic Toolkit FFS3** | Python GUI/core; **MIT** | Yes, live tree read | No | No | Secondary decoder/diagnostic for both endian orders; shares model heritage. |
| **MMI3G-Toolkit** | Python, **MIT** | Yes, automotive-specific scanning and extraction | No full writer | No | Samples with 256KiB units, unusual alignment and compression wrappers; limited directory reconstruction. |
| **PCM-Explorer** | Python, **MIT** | Yes, infotainment-specific EFS scanning | No | No | Additional vendor-specific compressed-file cases; not a full graph oracle. |
| **Audi MMI Studio `qnx_efs.rs`** | Rust, **MIT** | Partial parsing; inspect per version | **Experimental unqualified build method** | No | Reference for layout fields, but do **not** use builder output as native writer ground truth. |
| **`qnx_dumpers/efsdump.c`** | C, repository license not confirmed | Heuristic EFS extraction | No | No | Research lead only; **do not import code** or count as permissively licensed. |
| **`bigunclemax/mkxfs`** | C legacy source mirror; license not confirmed | No general reader | Potential flash-image assembly source, not qualified | No | Historical lead only; **do not import code** or claim BSD licensing. |
| **New `fsimg ffs3`** | C11, BSD-2-Clause | Target: yes | After native acceptance | **No** (by project scope) | User-facing image create, list, inspect, extract and verify. |

#### Portable read-only oracle: qnxmount (Linux)

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install qnxmount
# Linux: install the distribution FUSE userspace package separately.
mkdir -p /tmp/qnx-efs-ro
python3 -m qnxmount efs reference.efs /tmp/qnx-efs-ro
# In another shell:
find /tmp/qnx-efs-ro -print
# Unmount per your platform's FUSE method (e.g. fusermount -u or umount).
```

Sources: [qnxmount README](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/README.md), [EFS CLI](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/qnxmount/__main__.py), [EFS interface](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/qnxmount/efs/interface.py). It is **read-only** and the upstream project says it was tested on Linux. Its Kaitai schema fixes `endian: le`, so use it on **little-endian** images only; do not silently interpret big-endian images with that decoder.

#### Portable read-only oracle: qnxprobe (Python 3)

```sh
git clone https://github.com/abrignoni/qnxprobe.git
cd qnxprobe
python3 qnxprobe.py --self-test
python3 qnxprobe.py reference.efs
python3 qnxprobe.py --list reference.efs
python3 qnxprobe.py --extract extracted.zip reference.efs
# Also analyzes whole raw NOR flash dumps with multiple FFS3 partitions.
```

Source: [qnxprobe README](https://github.com/abrignoni/qnxprobe/blob/main/README.md) and [FFS3 research section](https://github.com/abrignoni/qnxprobe#qnx-efs-in-a-raw-flash-image-either-byte-order-and-deleted-data). It also offers forensic deleted-data analysis through its library and fixtures; do not conflate that with complete recovery of every historical file.

### 16.2 QNX host: generate an image with `mkefs`

**Preconditions:** an authorized QNX SDP host installation, activated license and QNX target or VM for subsequent native tests. Recent `mkefs` documentation says the binary checks for license rights. The command is documented to run on Linux/Windows hosts; older QNX SDP versions may use additional targets. [QNX mkefs manual](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkefs.html).

Create an *original* small buildfile (not copied from proprietary source):

```text
# case64.bld (your local test script data)
[block_size=64k spare_blocks=1 min_size=2M mount=/mnt/efs]
```

A simple empty-image oracle (native `mkefs` will add its own root and internal records):

```sh
mkefs case64.bld native-empty64.efs
```

A content-carrying image, using the documented `[buildfile] [directory] [outputfile]` argument roles:

```sh
mkdir -p src/etc src/log
printf 'FFS3 fixture\n' > src/etc/message.txt
: > src/etc/empty
printf '\000\001\377\000' > src/log/binary.dat
mkefs case64.bld src native64.efs
```

**Important input-path caveat:** when using the optional source-directory form, ensure the exact QNX SDP version accepts these operands and inspect `mkefs -?`; directory crawling, root naming and implicit parent creation can differ by version. For a version-independent oracle you can list source entries explicitly in a buildfile, with `-followlink` when testing links; never assume `mkefs` follows links the way fsimg does. The `mkefs -n` option makes timestamp handling more reproducible but **explicit `mtime=...` buildfile values are not stripped by `-n`**. Use fixed metadata and fixed source bytes when comparing exact image hashes.

Run matrix variations using *new* buildfiles, not undocumented CLI flags:

```text
[block_size=128k spare_blocks=2 min_size=4M mount=/mnt/efs]
[-bigendian]     # For LE, where applicable to the buildfile grammar
```

For big endian use the documented `[+bigendian]` attribute instead, subject to the chosen `mkefs` version. For ECC use `[ecc_on=1]` or `[ecc_on=2]` **only in a separate ECC qualification plan** with matching native `devf-*`/`flashctl` modes. The `block_size` must match the flash simulation's physical erase size. [mkefs attributes](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkefs.html).

### 16.3 QNX VM: mount generated image on emulated NOR

Use a **QNX VM and RAM-only flash device**. Never aim erase or format commands at physical automotive firmware/hardware during library testing. Device numbering/mountpoints depend on what else the VM has loaded. The following is a **test recipe to adapt on the target**, not an executable script guaranteed for all QNX versions:

```sh
# QNX target shell; start a 16 MiB simulated NOR, 64 KiB erase units.
devf-ram -s0,16m,,,64k -v -r &

# Verify this is the new RAM-backed socket before any destructive operation.
ls /dev/fs0*

# Erase the simulated raw socket (DESTROYS DATA ON /dev/fs0).
flashctl -p /dev/fs0 -ev

# Program a prebuilt mkefs/fsimg image into erased RAM flash.
# This QNX cp flag is used in qnxmount's native fixture generator.
cp -V native64.efs /dev/fs0

# The partition and mountpoint may appear automatically; otherwise use
# flashctl to mount the discovered formatted partition after checking ls.
flashctl -p /dev/fs0p0 -n /mnt/efs -m

flashctl -p /dev/fs0p0 -i
ls -la /mnt/efs
find /mnt/efs -print
sync
# Read the entire flash socket or exact partition after confirming device:
dd if=/dev/fs0 of=post-mount-nor.bin bs=64k
```

**Do not run `flashctl -f` after copying a preformatted image unless deliberately conducting a separate reformat experiment**: formatting it can replace the records whose compatibility you intended to test. For a *freshly created empty native partition* without copying a prebuilt image, QNX documents:

```sh
devf-ram -s0,16m
flashctl -p /dev/fs0p0 -e -f -m
```

These are **two separate workflows** (prebuilt-image import vs in-place native formatting). [QNX devf-ram manual](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/d/devf-ram.html), [flashctl manual](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/f/flashctl.html). The NFI [native test fixture builder](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/tests/qnx_efs/test_data/make_test_fs.sh) performs similar steps with 128 KiB units and additionally creates, overwrites, deletes, renames and snapshots files; use it as the starting *procedure*, not blindly as a script on an unknown host.

### 16.4 QNX VM: create mutation and recovery fixtures

On a mounted **RAM-only** FFS3 filesystem, construct the following cases:

```sh
mkdir /mnt/efs/d1
printf 'one\n' > /mnt/efs/d1/file
printf 'two\n' >> /mnt/efs/d1/file
cp /mnt/efs/d1/file /mnt/efs/d1/saved
mv /mnt/efs/d1/file /mnt/efs/d1/renamed
rm /mnt/efs/d1/saved
mkfifo /mnt/efs/d1/fifo
ln -s d1/renamed /mnt/efs/relative-link
# Use dd with a position/seek to update a bounded span in an existing file.
sync
flashctl -p /dev/fs0p0 -i
# Capture raw flash after each stage, plus an independent content/metadata log.
```

For **partial power-failure state**, take copies of the RAM flash at controlled stages *only if* you can instrument the driver to stop during a write. Random byte mutation of a correct image is a corruption/fuzz test, **not** a valid simulation of QNX's transactional interruption protocol. Never assert the writer has QNX crash-recovery guarantees merely because one byte-mutation case still mounts.

### 16.5 Independent reference images already available

- NFI's [QNX-generated 2 MiB EFS image](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/tests/qnx_efs/test_data/test_image.bin) produced with **128 KiB units** and [matching tar](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/tests/qnx_efs/test_data/test_image.tar.gz); the [generation script](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/tests/qnx_efs/test_data/make_test_fs.sh) includes large/small files, block overwrite, deletion, rename, symlinks, directories, permissions and FIFO.
- qnxprobe's [QNX-native-derived gzipped EFS fixture](https://github.com/abrignoni/qnxprobe/blob/main/tests/fixtures/efs-qnx.img.gz) and [tar](https://github.com/abrignoni/qnxprobe/blob/main/tests/fixtures/efs-qnx.tar.gz), derived from the NFI image rather than an independently generated second native one.
- qnxprobe's [little-endian synthetic image](https://github.com/abrignoni/qnxprobe/blob/main/tests/fixtures/efs-le.img.gz), [big-endian counterpart](https://github.com/abrignoni/qnxprobe/blob/main/tests/fixtures/efs-be.img.gz), [expected model](https://github.com/abrignoni/qnxprobe/blob/main/tests/fixtures/efs.known.json), and [MIT generator](https://github.com/abrignoni/qnxprobe/blob/main/tools/make_efs_fixtures.py). These test moved boot unit, reordered logical units, spare unit, multiple partitions, decoy magic, current/deleted/superseded data, and broken historical pointers. **They are synthetic parser oracles, not native mount-certified writers.**

When consuming third-party fixtures, record upstream repository, commit, download checksum, file license/provenance and whether reuse is allowed. A publicly hosted binary extracted from a commercial firmware should remain an **external locally supplied oracle**; do not vendor firmware under fsimg's BSD license.

---

## 17. Exact regression/qualification matrix

Each row is a test category, not merely a list of filenames. Store the fixture's native-version/profile/oracle provenance and expected hashes. An `fsimg` self-round-trip is useful but insufficient for the writer.

| ID | Fixture / operation | Expected assertion | Independent oracle |
|---|---|---|---|
| F01 | Default LE native `mkefs` empty volume | Recognized v3, root valid; zero user files; correct units/spare and 0xFF slack. | `mkefs`, `devf-ram`, `flashctl -i`. |
| F02 | LE 64 KiB, LE 128 KiB, LE 256 KiB | Same logical tree/content; correct distinct header locations and capacity. | Native `mkefs`/`devf-ram` per supported geometry. |
| F03 | BE 64/128 KiB | All integers decode in BE; same content/stat. | BE `mkefs` + compatible native driver and qnxprobe. |
| F04 | 4-, 32-, 64-byte text alignments | Correct shift formula; unsupported ECC explicitly diagnosed. | Synthetic LE/BE, then native ECC profile for the latter two. |
| F05 | 1 spare vs 2 spares | Live logical unit mapping is dense and spares aren't returned as files/free data. | Native `flashctl -i`. |
| F06 | Reordered physical units, boot in physical unit #2 or later | Root and paths unchanged; partition base recovered via unit_index. | qnxprobe MIT synthetic fixture; new native wear-level case. |
| F07 | Two FFS3 partitions in a socket, unformatted gap, early decoy magic | Scanner finds exactly two, no phantom third; ordinary single-volume open doesn't cross them. | qnxprobe MIT synthetic fixture. |
| F08 | Tiny file, zero-length file, all-zero file, all-`FF` file | Exact bytes / lengths; no erased-vs-zero confusion. | Native QNX tar + independent qnxmount. |
| F09 | File spanning multiple 4K and 16K extents, crossing physical units | Concatenation and random 1-byte reads match exact SHA-256; footer not read as data. | Native QNX reference and qnxmount/qnxprobe. |
| F10 | 1-byte, 2-byte and odd-length extent tails | No false content padding, correct encoded offsets. | Native `mkefs` or synthetic writer + oracle. |
| F11 | 0, 1, 254 and attempted 255 name-byte lengths | Correct stored namelen, padding and native acceptance boundary. | Native `mkefs`/`devf-ram` results **required**. |
| F12 | Deep tree, wide directory, duplicate names | Tree ordering deterministic; duplicate live name rejected; no quadratic memory blowup. | Native files plus fsimg namespace checker. |
| F13 | Relative and absolute symlink targets, empty and long targets | Stored byte target returned intact; no follow during extraction. | Native QNX links + QNXmount. |
| F14 | UID, GID, low mode 07777, parent mtime/ctime | Exact supported metadata; absent atime/birth time; files and directories. | Native `ls`/`stat` and tar. |
| F15 | FIFO and special node from mounted QNX driver | Correct recognized type or explicit unsupported; never device-file creation on host. | NFI 128 KiB native image. |
| F16 | Overwrite part of file, old data remains | Live file reads only replacement extents; old bytes reported only by optional forensic path. | NFI native image + qnxprobe recovery. |
| F17 | Delete and rename several files | Deleted files absent from live namespace; recoverable stale extents retain location/type. | Native QNX mutations and qnxprobe. |
| F18 | Trailing partially programmed header/status[1]/[2] conflict | No infinite loop; explicit status or unsupported state, no guess. | Mutated fixture and real native post-crash corpus if available. |
| F19 | Missing logical unit, duplicate identity, bad spare count | Detection fails with precise error; no out-of-bounds read. | Controlled synthetic modifications. |
| F20 | `next` and `super` cycles | Constant-space detection and work limit; return corruption. | Mutation/fuzzer. |
| F21 | Text offset huge, size 65535, overflowing last header | Integer-safety, legal max and overlap checks. | Property-based byte mutation. |
| F22 | All candidate signatures are decoys or displaced past scan budget | No phantom partitions; explicit no-match or `ELIMIT`. | Synthetic multi-partition decoy fixture. |
| F23 | Native flashctl after fsimg emission | Clean mount, expected `Spare`, `Headers`, `Padding`, no unexpected stale extents. | **Independent QNX runtime, writer release gate.** |
| F24 | QNX modifies fsimg volume and remounts | Old and new files readable, no corruption, reclaim works under spare policy. | **Independent QNX runtime, writer release gate.** |
| F25 | Reproducible mode `-R` with stable sources | Byte-identical images and deterministic metadata/header/padding; new writer must not copy host random IDs. | Two separate fresh fsimg runs + independent QNX mount. |
| F26 | Provider-generated zero ranges | Image stores 0x00 for file content, not erased 0xFF; full read equal to source. | Native mount/content compare. |
| F27 | 2GB and volume-size boundary (without generating full dense image in CI) | Exact size arithmetic, proper overflow refusal and support gate. | Plan-only tests and explicit large native integration job. |
| F28 | Bounded slice within an arbitrary larger disk | Never read outside `--offset/--length`; correct partition-relative offsets. | Guarded IO callback + decoy data outside slice. |
| F29 | Fuzzed invalid UTF-8 and path separator | No path traversal, no collisions caused by replacement characters. | fsimg safe extraction worker tests. |
| F30 | ECC-enabled native image | Recognizes profile and either correctly reads supported data or reports unsupported ECC validation; writer refuses profile. | Licensed QNX `mkefs` and `devf-ram -x1/-x2`. |

### 17.1 Deterministic manifest test

For every native-created image, capture a text manifest on the mounted QNX side and a host reference snapshot after safe transfer. Recommended per-entry fields:

```text
relative pathname (hex/escaped bytes to avoid ambiguity)
kind (regular, directory, symlink, FIFO, other)
file logical size
mode (octal), UID, GID
mtime seconds UTC, ctime seconds UTC where tooling exposes them
symlink target bytes
regular-file SHA-256 or an agreed platform-available exact-byte checksum
```

Make the manifest reproducible and explicit about tool limitations (`stat` may not be installed in all QNX versions). Compare filename bytes, hierarchy and payload *before* host filesystem timestamp restoration changes anything. The NFI test archive already compares tar metadata against the parser; supplement with a second independent file-content oracle for native writer qualification.

---

## 18. Fuzzing and specification-derived property tests

The existing fsimg project has format fuzz targets and discussions of Ruby-oriented spec-based testing. FFS3 is an especially good target because the independent fields have small, explicit ranges but the graph can be complex. Prefer **fast deterministic checks**, not a solver that explores full flash history.

### 18.1 Hand-written codec/property generators

Generate each record type independently in LE and BE from an original, permissively written model. Assert:

- Encode/decode round-trip on every supported scalar and pointer field; store **exact bytes** for reserved/opaque fields when round-tripping, with a fresh-image encoder that independently selects known legal defaults.
- `unit_base`, `header_at`, `text_at` calculations are monotonic, in range and unchanged by host byte order.
- `header_at(p,0) == end_of_unit-32`; increasing header index moves backwards 32 bytes.
- `text_at` respects its quantization, i.e. `(text_at-unit_base) % (1 << align_pow2) == 0`.
- Partition extents may never overlap the header region, even if `text_size == 0` or the number of extents is maximal.
- Equivalent LE and BE fixtures yield identical decoded semantic objects.
- Every encoded live reference points to one extent of a valid type; every allocated content text is reachable from at most its intended current graph owner.
- Byte mutations in a supported record either change an explicitly modeled field or cause a specific validation error; they never access memory outside input.

A Ruby integration test generator can write **declarative fixture models** (not a replacement for libFuzzer) using Ruby `pack`/`unpack`, **Minitest**, and a small property library if already approved in the test stack. Do not require Z3 for basic field bounds; state-space solving is optional for a small *layout planner* invariant model only.

### 18.2 Structured mutation matrix

Mutate targeted fields one at a time, recording the expected `FSIMG_*` error:

```text
unit_info.endian: valid L/B, invalid 00/FF
unit_info.unit_pow2: 0, 12, 16, 17, 18, 19, 31, 32, 65535
unit_info.erase_count: 0, 1, max u32
unit_logi.logical: 0, 1, N-R, N-R+1, FFFF, duplicate
boot.sig: intact, 1-byte difference, decoy unrelated to footer
boot.version: 3.0, 3.1, 2.0, 255.255
boot.unit_index: 0, N-1, N, FFFF
boot.total/spares: 0, 1, max, R=N, R>N
boot.align_pow2: 0, 2, 5, 6, 15, 16, FFFF
extent.status: erased, allocated, deleted, NO_NEXT/SUPER toggles
extent.text.offset: 0, end-1, end, end+1, shifted overflow
extent.text_size: 0, 1, 4096, 16384, 65535
extent.next/super: null, self, ancestor, dangling unit, spare unit
extent.type: SYS, DIR, FILE, unsupported
stat.size: 0, 19, 20, 21, FFFF
stat.mode: all supported kinds, unknown, invalid conflicting fields
dirent.namelen: 0, 1, 2, 254, 255, wrap and inconsistent NUL
record names: empty, . / .., slash, malformed UTF-8, high-bit, many combining sequences
physical: overlapping boot candidates, missing unit, duplicate unit, trailing short
```

Make mutations on both a **valid native QNX-generated fixture** and a **synthetic fixture**. A parser that accepts only its own writer's output can otherwise inherit correlated errors. Keep fuzz inputs small for continuous integration; long-lived fuzzing can target 64KiB/128KiB native captures.

### 18.3 Model-based graph generator

Use an abstract model containing `physical_units`, `logical_mapping`, `spares`, `extents`, `directory_entries`, `file_bytes`, `next`, `super` and `active`. Generate an acyclic valid namespace, then encode to a small synthetic image. Separately generate invalid near-neighbor graphs: next-loop, super-loop, dangling pointer, duplicate active directory name, unreachable object, deleted entry pointed at by a live chain, future-super transition, reordered physical blocks. Require fsimg read to agree with the model's expected live view. For corruption generators, require a *specific* error category rather than only absence of crash.

Avoid trusting the permissive qnxprobe synthetic writer as an independent implementation of these same tests when tests simply clone its model; combine its fixtures with QNX-native generated files. No automatic `rm`, truncate or memory image mutation operation in fsimg itself is necessary to exercise these tests.

### 18.4 Flash-specific correctness checks

- Every unused byte in a fresh image must be `0xFF`, including half-filled spare and empty areas, except for designated native system records.
- Every allocated file zero byte must be `0x00`, not the erased fill.
- Each extent header is placed at the *tail* of its unit and reflects exactly the planned text extent.
- All unit-info metadata and boot/root pointers are resolvable after simulated physical reordering (reader test).
- Spare units counted by boot metadata are not included in user capacity.
- Reproducible mode never inserts source `ctime` unless explicitly preserved under the accepted metadata policy.
- A false/unknown ECC mode may not lead to a `complete` verify report.

---
## 19. Adjacent format features and optional fsimg extensions

### 19.1 Full raw-flash socket exploration

A raw NOR dump frequently lacks a partition table, so a first-class scanner is useful. It could return a list of `fsimg_slice_t`-like descriptors (`start`, `length`, `format`, `geometry`, `confidence`, `boot_physical_unit`, `endian`) after searching candidate boots. It should use a general *reader scan interface* only if another existing format genuinely needs it; otherwise a tool-level explicit `ffs3` scanner is simpler. Do **not** invent a synthetic GPT/MBR to explain these partitions. An image may have boot code and deliberately blank gaps between FFS3 regions. The search can be parallel but must preserve deterministic candidate ordering and scan work budgets.

### 19.2 Raw flash image vs volume image

The fsimg CLI currently treats single mounted volumes and layered images separately. A good user workflow is:

1. Identify candidate FFS3 slices from a chip dump or obtain a known slice from external partition metadata.
2. Bound the target with `--offset` / `--length` and force or auto-select FFS3.
3. Run `i`, `t`, `W`, `x` against that slice.
4. If necessary, export a separate report of all physical units and stale-data provenance without altering the ordinary extracted tree.

This is preferable to making a driver silently scan and extract all embedded partitions into a fake root directory: it would violate fsimg's existing single-volume reader model and introduce path/offset ambiguity.

### 19.3 File-level QNX `deflate` payloads

Some FFS3 media hold files whose **content bytes** are QNX-compressed containers; older automotive tooling recognizes a marker `iwlyfmbp`. These are not a mandate for FFS3 to change logical extent concatenation. At baseline, fsimg returns those bytes as stored and exact size from the raw chain. A possible future `--decode-file-content=qnx-deflate` extractor filter would:

- identify and validate the wrapper separately (magic, version, blocks, lengths, supported codec);
- return both original and decoded content or use explicit opt-in semantics;
- enforce decompression output, per-file and work budgets;
- preserve exact stored bytes for verification and forensic use;
- document licensing of any decompression code independently.

Reference leads: [QNX `deflate`](https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/d/deflate.html), [PCM-Explorer EFS](https://github.com/dspl1236/PCM-Explorer/blob/a38709a0db90b9a3e70f4542b82553e93ace0f42/pcmexplorer/efs.py), [MMI3G format research](https://github.com/dspl1236/MMI3G-Toolkit/blob/e9ca3469835e3e1c78dabfb5cbe5676d2b752500/research/F3S_FORMAT.md). In particular, the MMI3G notes describe an `align_pow2=6` Plus variant that needs separate qualification; their `compressed F3S` label is an observation about those automotive images, not proof of a universal FFS3 compression mode.

### 19.4 Why not mount, or support in-place edits?

fsimg deliberately creates and reads images without mounting them. FFS3 updates are more complex than a FAT append because they require NOR erase/program rules, state transitions across redundant headers, wear-level and reclaim, atomically valid `super` chains, and power-failure recovery. A homegrown editor could silently destroy exactly the stale state that matters for forensics. No generic "mutable filesystem handle" is warranted for this project. A fresh writer is manageable and independently testable; edits should be performed through **native QNX** when needed for fixture generation.

### 19.5 QNX IFS, ETFS, QCFS and proprietary vendors

Keep a strict driver separation. A combined flash dump may contain QNX IFS boot images, ETFS NAND partitions and FFS3 NOR partitions. Do not recognize ETFS by `QSSL_F3S` or wrap it in this reader. A sophisticated scanner can present all recognized slices with distinct formats, but it must delegate to the appropriate driver. Hardware-proprietary flash pages with OOB ECC require a prior media-decoding layer before the filesystem reader can operate.

---

## 20. Permissively licensed implementations inspected: quality and limitations

### 20.1 NFI `qnxmount` (Apache-2.0)

- **Repository:** <https://github.com/NetherlandsForensicInstitute/qnxmount>
- **Kaitai layout:** <https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/qnxmount/efs/parser.ksy>
- **EFS logical walker:** <https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/qnxmount/efs/interface.py>
- **Linux FUSE mount:** <https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/qnxmount/efs/mount.py>
- **Fixture scripts and data:** <https://github.com/NetherlandsForensicInstitute/qnxmount/tree/main/tests/qnx_efs>
- **License:** <https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/LICENSE> (Apache-2.0).

**Strength:** clear structured field model and native-generated fixture whose content and metadata are compared against a tar tree. **Limits:** EFS parser is little-endian, native memory-mapped reader has host/offset assumptions, reads current live namespace, and its spare detection is a specific heuristic; not a general device firmware recovery model. A good reference for wire fields, not source to copy into a BSD driver mechanically.

### 20.2 `qnxprobe` (MIT)

- **Repository and user guide:** <https://github.com/abrignoni/qnxprobe>
- **FFS3 research and observations:** <https://github.com/abrignoni/qnxprobe#qnx-efs-in-a-raw-flash-image-either-byte-order-and-deleted-data>
- **Standalone parser and CLI:** <https://github.com/abrignoni/qnxprobe/blob/main/qnxprobe.py>
- **Synthetic image generator:** <https://github.com/abrignoni/qnxprobe/blob/main/tools/make_efs_fixtures.py>
- **Generated model and fixtures:** <https://github.com/abrignoni/qnxprobe/tree/main/tests/fixtures>
- **License:** <https://github.com/abrignoni/qnxprobe/blob/main/LICENSE> (MIT).

**Strength:** full-chip discovery, relocated boot records, LE/BE support, current namespace vs stale extent accounting, reasonably elaborate positive and negative test fixtures. The project reports testing 60 FFS3 partitions across six GM telematics NOR acquisitions, accounting their non-erased bytes and comparing extracted content with available forensic acquisitions; this is useful external evidence. **Limits:** native/forensic datasets may not be available for redistribution, synthetic writer does not mean a native-mountable flash formatter, and `qnxprobe`'s low-level schema descends from NFI's model, so this is *not entirely independent on the definition of fields*.

### 20.3 TRACE Forensic Toolkit (MIT)

- **Repository:** <https://github.com/Gadzhovski/TRACE-Forensic-Toolkit>
- **FFS3 parser:** <https://github.com/Gadzhovski/TRACE-Forensic-Toolkit/blob/67a2b96d69bebd01db29ded95c0254028f6bd99c/trace_app/core/qnx_efs.py>
- **License:** <https://github.com/Gadzhovski/TRACE-Forensic-Toolkit/blob/main/LICENSE> (MIT).

**Strength:** a second code path supporting both byte orders and live object traversal, tested against qnxmount/QNXprobe fixtures according to its own source. **Limit:** explicitly credits the same NFI format model, and does not qualify a writer or every physical recovery state. Treat as *secondary implementation checking*, not an independent format specification.

### 20.4 MMI3G-Toolkit (MIT)

- **Repository:** <https://github.com/dspl1236/MMI3G-Toolkit>
- **Detailed F3S format notes:** <https://github.com/dspl1236/MMI3G-Toolkit/blob/e9ca3469835e3e1c78dabfb5cbe5676d2b752500/research/F3S_FORMAT.md>
- **Walker / extractor:** <https://github.com/dspl1236/MMI3G-Toolkit/blob/e9ca3469835e3e1c78dabfb5cbe5676d2b752500/tools/walk_f3s_efs.py>, <https://github.com/dspl1236/MMI3G-Toolkit/blob/e9ca3469835e3e1c78dabfb5cbe5676d2b752500/tools/extract_f3s_efs.py>
- **License:** <https://github.com/dspl1236/MMI3G-Toolkit/blob/main/LICENSE> (MIT).

**Strength:** documented automotive Audi MMI3G real image subclasses with 256 KiB erase units, both `0x2C` and `0xC4` signature locations, stat records and QNX `deflate` wrappers. **Limit:** tool is aimed at its supplied firmware image class; some source methods scan for plausible dirent/stat patterns rather than reconstructing all FFS3 live extent-pointer relationships, and the Plus variant's compression is not handled. Useful for cross-checking *special cases*, not general correctness.

### 20.5 PCM-Explorer (MIT)

- **Repository:** <https://github.com/dspl1236/PCM-Explorer>
- **EFS parser:** <https://github.com/dspl1236/PCM-Explorer/blob/a38709a0db90b9a3e70f4542b82553e93ace0f42/pcmexplorer/efs.py>
- **License:** <https://github.com/dspl1236/PCM-Explorer/blob/main/LICENSE> (MIT).

**Strength:** testbed for automotive flash persistence images and compressed payload recognition (`iwlyfmbp`). **Limit:** a scanner can find plausible files but cannot be treated as a correct namespace/extent graph oracle. Extracting a blob by “bytes until the next dirent-looking record” is inherently unsafe for arbitrary binary file payloads. The correct fsimg driver follows explicit FFS3 links.

### 20.6 Audi MMI Studio Rust (MIT)

- **Repository:** <https://github.com/gballa/audi_mmi_studio>
- **FFS3 Rust parser and build prototype:** <https://github.com/gballa/audi_mmi_studio/blob/eb2fb0c3f28b8f65aeb76a4e2cbca784516c02e9/crates/mmi-formats/src/qnx_efs.rs>
- **Related schema notice:** the repository also includes `formats/qnx_efs.ksy`, whose *own metadata says* `license: Proprietary / Reverse-Engineered` despite the repository-wide MIT license. **Exclude this file from permissive code/source reuse** until its rights and provenance are clarified; its F3S/ETFS labeling is also overbroad. The writer discussion here is based on the separate Rust file and other permissively licensed references.
- **Tests:** <https://github.com/gballa/audi_mmi_studio/blob/eb2fb0c3f28b8f65aeb76a4e2cbca784516c02e9/crates/mmi-formats/tests/builder_tests.rs>
- **License:** <https://github.com/gballa/audi_mmi_studio/blob/main/LICENSE> (MIT at repository level; bundled `.ksy` metadata caveat follows).

**Strength:** an inspectable MIT Rust experiment for the usual 16/24/24/8/20-byte structures, unit geometry and root/mountpoint conventions. **Do not incorporate the bundled Kaitai schema**, which separately self-labels its licensing as proprietary/reverse-engineered. **Critical limit:** the inspected `build()` method writes bootstrap records and file-like content near the front of units but does **not** visibly serialize the required backward 32-byte extent-header table at each unit tail. Therefore it must **not** be cited as proof that a general usable FFS3 creator exists. Require native mount/modify test before giving it authority.

### 20.7 Research leads **not** approved for code reuse

- [`jtang613/qnx_dumpers`](https://github.com/jtang613/qnx_dumpers), with `efsdump`/`ifsdump` tools. A clearly applicable permissive license was **not established** during this research. Treat as an externally linked lead only, not a source of code, wire structure definitions or copied algorithms for fsimg.
- [`bigunclemax/mkxfs`](https://github.com/bigunclemax/mkxfs). A clearly applicable permissive license was **not established**; historical build tooling may derive from proprietary QNX code. Do **not** copy or port it.
- Third-party GitHub mirrors containing SDK paths such as `fs/f3s_spec.h` or `mk_flash_fsys.c`. A publicly visible copy of a proprietary QNX development header **does not make it permissively licensed**. No verbatim excerpts from such mirrors are needed by this specification.
- QNX licensed binaries (`mkefs`, `devf-ram`, `flashctl`) are **test tools only** and are **not** vendored to fsimg, disassembled or reproduced in this work.

---

## 21. Source and license inventory

The table below is the intended link/source manifest for an engineering PR. “Permissive” here refers to availability to read/derive from the stated open-source license, **not** permission to change fsimg's own BSD-2-Clause policy; even when license permits reuse, no external implementation code is being copied into this spec.

### 21.1 QNX primary public **documentation** (interface and behavior only)

| Reference | What it establishes | License/reuse boundary |
|---|---|---|
| [QNX 8 FFS3 architecture](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/fsys_FFS3.html) | NOR design, erase/reclaim/wear-leveling, 32-byte extent headers, 512-byte append buffer, POSIX exceptions, runtime extent sizes, atomic rename. | QNX documentation; cite behavioral facts, do not copy documentation verbatim. |
| [QNX 8 FFS3 user guide](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.user_guide/topic/fsystems_Flash.html) | Device-driver integration, format name and missing `.`/`..` entries. | QNX documentation. |
| [QNX 8 filesystem feature matrix](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/fsys_Limitations.html) | No atime/hard links, yes directories/symlinks/permissions/mtime/ctime. | QNX documentation. |
| [QNX flash filesystem limits](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.user_guide/topic/limits_Flash.html) | 255-byte filename, 1024-byte path, 2 GB minus 1 volume/file limit. | QNX documentation. |
| [QNX 8 `mkefs` manual](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkefs.html) | v3 image creation, host runs, licensing, input grammar, byte order, unit size, spare counts, ECC, `min_size`, metadata. | QNX documentation; binary is proprietary. |
| [QNX 8 `devf-ram` manual](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/d/devf-ram.html) | In-RAM NOR mount simulation, geometry, `-u`, `-x`, no built-in decompression. | QNX documentation; binary is proprietary. |
| [QNX 8 `flashctl` manual](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/f/flashctl.html) | Erase/format/mount; `-b` ECC alignment; `-i` space and spare diagnostics. | QNX documentation; binary is proprietary. |
| [QNX `deflate` utility](https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/d/deflate.html) | Independent QNX compressed file handling. | QNX documentation; separate content filter only. |
| [QNX 6.6 FFS3 architecture](https://qnx.com/developers/docs/6.6.0_anm11_wf10/com.qnx.doc.neutrino.sys_arch/topic/fsys_FFS3.html) | Older behavior of FFS3 and deflate/inflator, support changes across releases. | QNX documentation. |

### 21.2 Permissive **code, tests and fixtures**

| Reference | License | Recommended use |
|---|---|---|
| [NFI qnxmount](https://github.com/NetherlandsForensicInstitute/qnxmount) | Apache-2.0 | Wire structure model, LE reference reader, native QNX fixture and tar. |
| [NFI FFS3 `parser.ksy`](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/qnxmount/efs/parser.ksy) | Apache-2.0 | Record offsets, bit fields, pointer/endian interpretation; hand-write fsimg codec. |
| [NFI EFS test fixture script](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/tests/qnx_efs/test_data/make_test_fs.sh) | Apache-2.0 project | Native QNX create/mutate/capture **test procedure**. |
| [NFI EFS image/tar](https://github.com/NetherlandsForensicInstitute/qnxmount/tree/main/tests/qnx_efs/test_data) | Published in Apache project, image produced by QNX | External test corpus; establish redistribution rights separately before vendoring bytes. |
| [qnxprobe](https://github.com/abrignoni/qnxprobe) | MIT | Cross-platform second reader and raw socket forensics. |
| [qnxprobe FFS3 synthetic fixture generator](https://github.com/abrignoni/qnxprobe/blob/main/tools/make_efs_fixtures.py) | MIT | Expected-good and malformed profile fixtures for byte/endian/recovery, **not** native writer certification. |
| [TRACE EFS parser](https://github.com/Gadzhovski/TRACE-Forensic-Toolkit/blob/67a2b96d69bebd01db29ded95c0254028f6bd99c/trace_app/core/qnx_efs.py) | MIT | Secondary cross-check on live tree, LE/BE. |
| [MMI3G FFS3 format notes](https://github.com/dspl1236/MMI3G-Toolkit/blob/e9ca3469835e3e1c78dabfb5cbe5676d2b752500/research/F3S_FORMAT.md) | MIT project | Automotive geometry variations, deflate wrappers, current empirical gaps. |
| [PCM-Explorer EFS scanner](https://github.com/dspl1236/PCM-Explorer/blob/a38709a0db90b9a3e70f4542b82553e93ace0f42/pcmexplorer/efs.py) | MIT | Additional special-case heuristic research only. |
| [Audi MMI Studio F3S Rust experiment](https://github.com/gballa/audi_mmi_studio/blob/eb2fb0c3f28b8f65aeb76a4e2cbca784516c02e9/crates/mmi-formats/src/qnx_efs.rs) | MIT repository-level | Field comparison and *unvalidated* writer prototype; not native oracle. **Do not reuse** the separate bundled `.ksy` schema, which self-describes as proprietary/reverse-engineered. |

### 21.3 Relevant fsimg files (project's own BSD-2-Clause implementation)

| fsimg component | Why it matters |
|---|---|
| [Repository README](https://github.com/stass/fsimg/blob/master/README.md) | Product, CLI, supported platforms and current formats. |
| [Developer conventions](https://github.com/stass/fsimg/blob/master/CLAUDE.md) | BSD-only code and build/test conventions. |
| [Public API header](https://github.com/stass/fsimg/blob/master/include/fsimg.h) | Reader/write options, capability bits, error taxonomy, volume and time metadata. |
| [Architecture](https://github.com/stass/fsimg/blob/master/specs/architecture.md) | No mounts, arbitrary read-only slices, independent resource budgets, staging and sandbox. |
| [Format contracts](https://github.com/stass/fsimg/blob/master/specs/design-formats.md) | Static descriptors, bounded prefix probe, private unqualified writer path. |
| [Format private descriptor interface](https://github.com/stass/fsimg/blob/master/lib/formats/format.h) | Pure probe signature and independently optional read/write methods. |
| [Image emission](https://github.com/stass/fsimg/blob/master/specs/design-emission.md) | Current **zero-fill assumption** requiring safe generic extension for FFS3. |
| [Storage and algorithms](https://github.com/stass/fsimg/blob/master/specs/design-storage.md) | Bounded extents, metadata spill, source-copy and verification. |
| [Content ranges](https://github.com/stass/fsimg/blob/master/specs/design-content.md) | `read_at`/`next_range`, initialized ranges, shared source-copy contract. |
| [Prepared directories](https://github.com/stass/fsimg/blob/master/specs/design-directories.md) | Namespace validation, bounded traversal and duplicate detection. |
| [POSIX metadata model](https://github.com/stass/fsimg/blob/master/specs/design-metadata.md) | Supported FILE/DIR/SYMLINK/FIFO, UID/GID/mode, changed-time semantics. |
| [CLI](https://github.com/stass/fsimg/blob/master/specs/design-cli.md) | Actual `-F`, `-o`, `--offset`, `--length`, `-R`, `-s`, `-W` contracts. |
| [QNX4 driver](https://github.com/stass/fsimg/blob/master/specs/design-qnx4-driver.md) | Model for separate format research, reader, writer and native test gates. |
| [QNX6 driver](https://github.com/stass/fsimg/blob/master/specs/design-qnx6-driver.md) | Model for unqualified writer descriptor, stream layouts and validation. |
| [Existing upstream spec directory](https://github.com/stass/fsimg/tree/master/fsspecs) | Suggested eventual home for this research specification. |
| [Existing project specs](https://github.com/stass/fsimg/tree/master/specs) | Authoritative target once unresolved issues are validated. |

**Provenance rule for implementation PRs:** create original C codecs from this independently written format description; cite the permissive NFI schema/QNX docs in comments or design notes. Avoid copying a source file from an incompatible or unknown-license repository, including copying “only the constants” as an unreviewed header; exclude source files with contradictory license metadata regardless of the containing repository’s nominal license. Keep borrowed snippets, if any are ever approved later, within their license obligations; the initial driver should need no borrowed implementation source.

---

## 22. Open research questions and acceptance gates

Ranked by impact on reader/writer correctness:

| Priority | Unresolved question | Concrete native or external experiment | Safe interim choice |
|---|---|---|---|
| **P0** | Which exact `status[0]` condition values are valid for ordinary, GC and incomplete writes? | Compare fresh, overwritten, renamed, deleted, reclaimed and crash-interrupted `devf-ram` images. | Reader supports proven conditions; report unknown active states as unsupported. No writer before native status qualification. |
| **P0** | What do status[1] and status[2] mean; when can they differ? | Diff native outputs under controlled write/interruption. | Keep raw fields, do not compare equal as a universal invariant. |
| **P0** | How are non-ECC headers, ECC field bytes and `unit_logi` trailing 16 bytes initialized and validated? | Inspect QNX mkefs output with ECC off, format/mount and native `flashctl -i` state; then ECC modes separately. | ECC-off writer gated; don't fabricate cryptographic digest. |
| **P0** | How exactly are spare units serialized and selected after wear-level relocation? | Create 1- and 2-spare QNX images, exercise reclaim then compare `unit_info`, status1 and active logical map. | Reject ambiguous active map; no wear-level writer logic. |
| **P0** | Does a minimal independently written image actually mount and remain writable under native `devf-ram`? | Program into *erased RAM flash*, inspect `flashctl`, add and read files, remount. | Writer is private unregistered until it does. |
| **P0** | Can the present fsimg emission core support non-zero fill *without* breaking ZERO-source semantics? | Generic emitter unit tests and FFS3 fresh-image tests. | Require a small shared fill/copy contract update before writer work. |
| **P1** | How to recognize relocated boot record without violating bounded prefix probe? | Refactor spec in §11.3 and test synthetic relocated boot/decoys/multiple partitions. | Only claim provable early identity; forced deep scan is the preferred extension. |
| **P1** | Are 255-byte names really serializable in the observed `u8 namelen including NUL` layout? | `mkefs` and native `creat`/`rename` with 253, 254, 255-byte names and byte-level dump. | Conservative 254-byte writer cap. |
| **P1** | Root/mount path semantics: empty name, mountpoint string, first child pointer on empty FS? | Compare native mkefs mount= variants and the NFI test image. | Root is implicit; only established encoding in initial writer. |
| **P1** | What is the on-flash representation of a zero-length regular file, empty symlink, empty dir and FIFO? | Controlled native QNX files, inspect the full dirent + extent state and tar contents. | Reader handles reference profile; writer refuses unqualified edge kinds. |
| **P1** | Are true BE FFS3 images writable/mountable on an available QNX emulator? | Use `mkefs +bigendian` and a compatible BE target/driver; distinguish parser from mount oracle. | BE **read**, native BE writer unqualified. |
| **P1** | Are directories and files always represented by distinct `TYPE=DIR` vs `TYPE=FILE` extents in all supported versions? | Cross-check native profiles and newer FFS3 images. | Support observed v3.0 profile only; no heuristic scanning. |
| **P1** | Does `age` distinguish active copies after reclaim and duplicate logical IDs? | Capture on successive GC passes and compare boot/map. | Reject duplicate live logical IDs instead of choosing by age. |
| **P1** | What is the correct exact `EXT_LAST`/erased sentinel interaction? | Diff unit tails from native mkefs, devf-ram before/after allocation. | Strict bounded table scan, accept proven terminal encodings. |
| **P2** | How does QNX encode characters outside ASCII? | Native UTF-8 and invalid-name experiments. | Treat bytes as untrusted; strict API Unicode validation without lossy repairs. |
| **P2** | What does `moves` do, and does it affect live selection? | Native repeated `rename` samples. | Preserve/report but do not infer ordering. |
| **P2** | Is `ctime` interpreted as unsigned, signed or internally normalized after year 2038? | Native time metadata near boundary. | Round-trip raw u32; do not overpromise temporal interpretation. |
| **P2** | Are there later FFS3 minor versions/embedded extension records? | Acquire documented/authorized real devices, compare version bytes. | `EUNSUPPORTED` for unknown major or new extension that changes content. |
| **P2** | Is transparent decompression needed for any FFS3 profile? | Characterize concrete image; distinguish filesystem codec vs ordinary `deflate` file bytes. | Return raw file bytes; separate optional codec. |
| **P2** | Can file extent text exceed QNX's runtime 4KiB / mkefs 16KiB commonly? | Native build various file sizes and inspect header `text_size`. | Read full u16 safely; writer only qualified chunk sizes. |
| **P2** | Reproducible timestamps: does QNX mkefs preserve user `ctime` separately from `mtime`? | Compare `-n`, buildfile mtime and native `stat`. | Writer uses explicitly reproducible epoch for both until characterized. |

This inventory is intentionally explicit. A structural reader can often be useful with a few unverified fields because it never changes bytes. A **fresh image creator** must meet a much higher standard: it should generate objects the native runtime can parse, modify, reclaim and remount, not merely objects the same project's reader accepts.

## 23. Definition of done, release claims and next steps

**A complete release-quality read-only FFS3 driver** has passed the NFI native image/tar test, LE/BE parser fixtures and moved-boot tests, corpus fuzzing, payload/namespace checks, and all existing fsimg extraction and resource/security tests. It must expose clearly qualified profiles through `fsimg --formats` and explain rejected metadata/ECC/recovery states. Automated tests must not require QNX licenses; native QNX runs are separately labeled integration gates.

**A complete fresh-image FFS3 writer** additionally needs reproducible FF-filled bytes, safe zero-content copying, geometry/spare accounting, independent `qnxmount`/`qnxprobe` comparison, successful native `devf-ram` mount and arbitrary ordinary file operations/remount, plus a written compatibility matrix of each tested endian/unit-size/alignment combination. No claims of hardware flashing or crash-safe incremental updates.

**Recommended first engineering sequence:**

1. Commit this as **research** (for example `fsspecs/QNX_FFS3_fsimg_spec.md`) and capture a pinned corpus manifest. Do not treat it as the authoritative coding contract until the open native-write questions are answered.
2. Implement and fuzz the six record codecs; write a tiny read-only `ffs3-inspect` capability behind internal tests to parse mapped units/root/extents.
3. Finish and integrate the read-only driver and structural verifier; qualify its public descriptor against NFI/QNXprobe and native QNX fixture.
4. Design the **generic nonzero erased-fill** and zero-source handling extension with separate tests for every existing fsimg writer.
5. Build a restricted experimental FFS3 writer, then validate it by actually mounting and mutating in QNX `devf-ram`. Only then register public `FSIMG_CAP_WRITE` and `cW` support.
6. Later add multi-partition NOR scanning, optional stale-data recovery and specific ECC/compressed profiles **only after new evidence**.

**Bottom line:** the available Apache-2.0 and MIT sources are sufficient to specify a serious FFS3 **read-only parser and verifier**, including precise binary records and live-chain semantics. A **native-compatible writer** is achievable in stages but is not fully specified by public evidence alone: status transitions, redundant status words, spare serialization and ECC behavior still require licensed native QNX tooling for qualification. The uniquely important fsimg-wide prerequisite is replacing the writer's hardwired `0x00` destination initialization with a per-format fill contract that correctly handles `0xFF` erased NOR and explicit zero-valued file content.
