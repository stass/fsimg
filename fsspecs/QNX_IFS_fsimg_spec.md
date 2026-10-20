# QNX IFS / ImageFS — interoperability specification and fsimg implementation plan

**Research date:** 19 October 2026  
**Target project:** `fsimg` (filesystem-image creation and extraction)  
**Status:** Engineering specification, with explicitly identified verification gaps. This is **not** a vendor-conformance certificate.

> **Provenance and licensing rule.** The narrative, diagrams, validation rules, proposed interfaces, and examples below were written independently for this document. Binary field names, sizes, offsets and signatures are interoperability facts, not copied implementation code. **No QNX source code, proprietary source, GPL/AGPL code, or MIT-licensed code has been reproduced here.** BSD-licensed format readers and compression decoders are identified separately, with license/provenance caveats and attribution requirements. Other repositories are linked *for external comparison only* and must not be copied into a BSD-only `fsimg` implementation without a separate licensing decision. QNX's own documentation and binaries remain subject to their terms.

## 0. Executive summary

QNX **IFS** is the *Image File System*, an immutable filesystem image loaded at system initialization, normally exposed at `/proc/boot` (or another configured mount path). It is **not** QNX4, QNX6 Power-Safe, EFS/ETFS, or a conventional writable disk filesystem. It has a flat directory-record table, contiguous file data, an image header and trailing checksum; bootable variants also have a boot prefix, a 256-byte startup header, startup program, and a second checksum. Entire image filesystem bodies can be compressed.

Recommended scope:

1. **Reader:** standalone uncompressed ImageFS; bootable IFS detection and extraction; little/big-endian headers; regular files, directories, symbolic links, special nodes; metadata and checksum verification.
2. **ImageFS creator:** deterministic **nonbootable** read-only ImageFS from a tree/manifest. Validate with official `dumpifs` and `mount_ifs`; this is an attainable writer without needing to generate a proprietary startup program.
3. **Bootable preservation/repack:** preserve externally supplied boot prefix and startup section *as opaque bytes*; update known length fields and checksums where independently validated. Treat compression, executable layout, physical addresses, and security metadata as compatibility-sensitive.
4. **Optional advanced work:** compressed writers (UCL, LZO, zlib); startup script compiler; board-specific wrapping; digitally signed images; IFS checksum-array (IFSCSA). Do not promise firmware bootability based only on a readable `dumpifs` listing.

**Best independent references:** official QNX [`mkifs`](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkifs.html), [`dumpifs`](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/d/dumpifs.html), [startup-header manual](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.building/topic/ipl/ipl_startup_header.html); [BSD-2-Clause `lclevy/dumpifs`](https://github.com/lclevy/dumpifs); the [BSD-3-Clause independent LZO1X decoder](https://github.com/go-compressions/lzo); and the independent [MIT `qnxprobe`](https://github.com/abrignoni/qnxprobe) and [MIT `qnx_dumpers`](https://github.com/jtang613/qnx_dumpers) for behavior comparison, **not code reuse**.

### Evidence levels used here

| Label | Interpretation |
|---|---|
| **D** | Published by QNX in public documentation (especially versions 7.0/7.1/8.0). |
| **O** | Observable byte-format fact independently exposed by BSD reader(s), third-party readers, and/or a public diagnostic fixture. |
| **P** | Proposed `fsimg` policy or safe-writer design, **not** an on-disk requirement. |
| **V** | Requires real fixtures and differential testing before it should be considered interoperable. |

All offsets in the tables are **hexadecimal**, are relative to the start of the named structure unless noted, and refer to the **decompressed** filesystem body where appropriate. `u16`, `u32`, `u64` mean fixed-width unsigned integers; they are **not** the host compiler's `unsigned long`.

---

## 1. Format identity, scope and terminology [D/O]

* **Boot image/IFS:** loadable package containing boot machinery (if bootable) and an ImageFS body. Extension `.ifs` does not establish its *container format*: QNX System Builder also uses `.ifs` for ELF, S-record and other filtered output.
* **ImageFS:** the filesystem body, beginning with seven ASCII bytes `imagefs`. QNX identifies it with a fixed signature, followed by a header and path-oriented records.
* **Bootable IFS:** has a startup header, startup code and a startup trailer. It may include a preboot prefix/wrapper.
* **Nonbootable IFS:** omits startup components. It can be built with `mkifs` and mounted as an additional filesystem.
* **Imagefs mountpoint:** an image-header setting, distinct from a buildfile's `prefix` used to name entries. Common boot setup maps files under `/proc/boot`.
* **ImageFS is read-only at runtime**; “write support” in `fsimg` means offline creation, **not** a general-purpose in-place writable filesystem.
* **Embedded component vs container:** a QNX IFS can appear inside raw NAND/eMMC dumps, disk partitions, UEFI/ELF files or OEM updater packages. Do not conflate an OEM wrapper with the IFS format.

Official context: [QNX 7.1 OS images](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.building/topic/os_images/images_about.html), [QNX 8 filesystem architecture](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/fsys.html), [QNX 8 `mkifs`](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkifs.html).

## 2. Logical structure [D]

**Bootable, noncompressed:**

```text
+-----------------------------------+
| optional preboot / loader wrapper |  arbitrary, platform-dependent
+-----------------------------------+
| startup header                    |  256 bytes at startup_base
+-----------------------------------+
| startup machine code and data     |  boot-specific bytes
+-----------------------------------+
| startup checksum trailer          |  4 bytes, normally at end of startup_size
+-----------------------------------+
| ImageFS header: 'imagefs'         |  body_base = startup_base + startup_size
| mountpoint, directory records     |  offsets relative to body_base
| file data, padding and alignment  |
| ImageFS checksum trailer          |  4 bytes, included in image_size
+-----------------------------------+
```

**Bootable, compressed:** the startup section stays uncompressed; after it comes a **compressed representation of the entire ImageFS body**, including its metadata and its own image checksum. A reader decompresses this payload *before* parsing any ImageFS directory records.

**Nonbootable:** starts directly with the ImageFS body (subject to external wrapping). It has no startup header, startup binary, or startup trailer. A raw nonbootable `mkifs` output is the simplest interoperability target.

**Important length convention:** `startup_size` is the byte count **from startup-header base to the beginning of the stored filesystem payload**. It covers the 256-byte startup header and subsequent startup material. The `dumpifs` *displayed* line for `startup.*` begins after the header and can show a smaller value; do **not** add `header_size` again when seeking to compressed data. This relationship is exercised by third-party readers on real images and is consistent with QNX's published example output. For a prefixed image, use `startup_base + startup_size`, not `file_offset = startup_size`.

QNX defines `stored_size` as the stored image's size including its startup header. In ordinary contiguous examples, `stored_size - startup_size` is the stored filesystem body length. The difference can change when proprietary wrapping/filtering is involved: validate exact byte bounds, not a guessed padding rule. [QNX startup fields](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.building/topic/ipl/ipl_startup_header.html), [QNX `dumpifs` sample](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/d/dumpifs.html).

## 3. Startup header (bootable images) [D/O]

### 3.1 Detection and byte order

* Native numeric signature `0x00FF7EEB` at offset `0x00`.
* Raw bytes for common **little endian**: `EB 7E FF 00`.
* Raw bytes for **big endian**: `00 FF 7E EB`.
* Standard header length **256 bytes** (`0x100`).
* Version is typically **1**; do not reject every other version *solely* by an equality assertion when doing forensics. Reject unsupported variants if their geometry cannot be validated.
* Confirm a candidate using signature **and** plausible size fields, known compression values, bounds, and a valid `imagefs` structure after decompression. A magic-byte search by itself is unsafe.

### 3.2 Core fields (offset from startup header)

| Byte offset | Type | Field | Meaning / parser action |
|---|---|---|---|
| `0x00` | u32 | signature | `0x00FF7EEB` in image's native endian. |
| `0x04` | u16 | version | Image-builder/startup format version. |
| `0x06` | u8 | flags1 | Endian, virtual and compression indicators. |
| `0x07` | u8 | flags2 | Reserved in published versions; preserve. |
| `0x08` | u16 | header_size | Normally `0x100`. |
| `0x0A` | u16 | machine | ELF e_machine architecture number. |
| `0x0C` | u32 | startup_vaddr | Startup entry address (address, **not** file offset). |
| `0x10` | u32 | paddr_bias | Target address mapping bias. |
| `0x14` | u32 | image_paddr | IPL/startup physical-address metadata. |
| `0x18` | u32 | ram_paddr | Where startup expects to be copied. |
| `0x1C` | u32 | ram_size | Runtime image RAM footprint, may differ from stored bytes. |
| `0x20` | u32 | startup_size | Byte span from startup-header base through startup section; filesystem payload begins at its end. |
| `0x24` | u32 | stored_size | Stored image span including startup header. |
| `0x28` | u32 | imagefs_paddr | Physical address for filesystem body (not a container offset). |
| `0x2C` | u32 | imagefs_size | Decompressed ImageFS byte count. |
| `0x30` | u16 | preboot_size | Loader-side preboot span; not a universal file locator. |
| `0x32` | u16 | zero0 | Reserved, keep original. |
| `0x34`..`0xFF` | variant | tail | Described below; preserve raw bytes. |

### 3.3 Header-tail version difference (important)

**Older documented form (QNX 7.0):** `0x34..0x3F` reserved (`3 × u32`); `0x40..0xFF` startup info array (`48 × u32`). **No 64-bit address offset field** in this published layout.

**QNX 8.0 published form:** `0x34..0x37` reserved (`1 × u32`); `0x38..0xF7` startup info array (`48 × u32`); `0xF8..0xFF` 64-bit address extension (`addr_off`). QNX says this extension is added to the relevant 32-bit address fields to support physical/virtual addresses above 4 GiB.

**Do not infer which form is present only from magic=1.** There is no documented universal discriminant exposed in these pages. Parse shared fields by the core layout; treat the tail as opaque for portable reading; use a selected producer profile or QNX SDK fixture to interpret it. In a new 8.0 image, apply `addr_off` to its specified address fields **only when interpreting target addresses**, never when interpreting file-relative offsets.

Sources: [QNX 7.0 startup header](https://get.qnx.com/developers/docs/7.0.0/com.qnx.doc.neutrino.building/topic/ipl/ipl_startup_header.html), [QNX 8.0 startup header](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.building/topic/ipl/ipl_startup_header.html).

### 3.4 `flags1` bit values

| Mask/value | Interpretation |
|---|---|
| `flags1 & 0x01` | MMU-enabled/virtual boot image. |
| `flags1 & 0x02` | Big-endian startup image when set. |
| `flags1 & 0x1C == 0x00` | Uncompressed ImageFS. |
| `== 0x04` | ZLIB. |
| `== 0x08` | LZO. |
| `== 0x0C` | UCL (UCL8 NRV2B). |
| other values of `& 0x1C` | Unknown/reserved compression; explicit unsupported-format result. |
| bits `0x20`, `0x40`, `0x80` | Preserve; do not ascribe semantics without fixtures. |

Do not treat the compression value as three independent booleans. `machine` common values: **3** x86, **40** ARM32, **62** x86-64, **183** AArch64. Further ELF architectures may occur in historical QNX versions; do not hardcode this set as exhaustive. [QNX startup documentation](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.building/topic/ipl/ipl_startup_header.html).

### 3.5 Startup checksum and startup-specific material [D/V]

The startup section terminates with a **4-byte startup checksum**. Official documentation states its existence. The exact checksum domain, padding and whether the startup header's mutable IPL communication fields are excluded/normalized must be established with a specific fixture and compared with `dumpifs -vv`; **do not assume** the ImageFS checksum calculation also applies to startup. Preserve the original startup bytes and checksum when doing read-only extraction or an ImageFS-only rewrite; only rewrite checksum after confirming the algorithm.

The opaque startup region may contain a board-specific bootloader stub, executable initialization code, and special relocation/linker arrangements. `fsimg` need not disassemble or generate it to support filesystem reading/creating.

## 4. Uncompressed ImageFS header [O]

All fields below are **relative to the start of the decompressed ImageFS body**, denoted `B`. The ImageFS header begins with a **7-byte** ASCII signature `imagefs` (without an eighth terminator byte).

| `B+` offset | Type | Meaning |
|---|---|---|
| `0x00` | bytes[7] | `imagefs` magic. |
| `0x07` | u8 | ImageFS header flags. |
| `0x08` | u32 | `image_size`: byte count from `B` through trailing image checksum. |
| `0x0C` | u32 | `hdr_dir_size`: offset of end of directory table, from `B`. |
| `0x10` | u32 | `dir_offset`: offset of first directory record, from `B`. |
| `0x14` | u32[4] | `boot_ino`: up to four bootstrap-program inode references. |
| `0x24` | u32 | `script_ino`: inode reference for compiled startup script. |
| `0x28` | u32 | `chain_paddr`: physical pointer/address to another IFS, not an arbitrary file offset. |
| `0x2C` | u32[10] | Reserved fields; preserve. |
| `0x54` | u32 | Default mount options/flags (QNX mount semantics). |
| `0x58` | bytes, NUL terminated | Default mount path; variable-length and may be empty. |
| after path | padding | Leads up to `dir_offset`; do not assume one fixed header length beyond `0x58`. |

Header flag bits observed in publicly accessible format descriptions:

| Flag | Meaning |
|---|---|
| `0x01` | Header, directory and image trailer numeric fields are big-endian. |
| `0x02` | Read-only image mapping. |
| `0x04` | High inode bits have defined executable/loader attributes. |
| Other bits | Unknown; retain, and refuse potentially unsafe rewrite. |

**Endianness:** parse `flags` (single byte) before multibyte numeric values. For a bootable IFS the startup and ImageFS endianness should be mutually consistent; if they differ, report a structural error or an explicit exceptional-case warning instead of silently misparsing. For a standalone ImageFS, its own flag is the endian source.

**Numeric-width caution:** on-disk header numbers are **32-bit**, regardless of the reader running on x86-64 or ARM64. Some historical public headers use the language-level name `unsigned long`; do not serialize the host's 64-bit `long`.

**Bounds checks:**

```text
image_size >= 0x58 + minimum_mountpoint_and_checksum_bytes
image_size <= decompressed_available_bytes
0x58 <= dir_offset <= hdr_dir_size <= image_size - 4
mountpoint_NUL_position < dir_offset
image_size % 4 == 0      (needed for the standard u32 additive checksum)
```

Treat these as the strict *standard-profile* checks; a forensic/relaxed reader may retain malformed evidence without extracting untrusted file content. The image's own numeric size and the startup header's `imagefs_size` should normally agree; mismatches must be reported.

## 5. ImageFS directory encoding [O]

Directories are stored as a **linear table of variable-sized entries**. Each entry identifies a full path relative to the ImageFS root. There is no allocation bitmap, journal, B-tree, block mapping, or separate hierarchy of child directory pages.

### 5.1 Common directory-record prefix (24 bytes)

At the offset `R` of each directory entry:

| `R+` | Type | Description |
|---|---|---|
| `0x00` | u16 | Record byte length, includes prefix, type-specific data, name, target (if any), padding and extensions. |
| `0x02` | u16 | Optional extended-attribute offset within the record; `0` means no extension. |
| `0x04` | u32 | Inode number/flags; `0` means an unused/deleted/ignored record. |
| `0x08` | u32 | POSIX/QNX mode word: type + permission + special permission bits. |
| `0x0C` | u32 | Group ID. |
| `0x10` | u32 | User ID. |
| `0x14` | u32 | Modification time in Unix seconds. |

Parse the type from `mode & 0xF000`. Continue with `R += record_length` only if `record_length >= 24`, the length fits wholly inside the declared directory table, and the type-specific minimum is satisfied. A zero-length header or reaching `hdr_dir_size` ends the table. **Do not** advance with zero size; it would cause an infinite loop. Preserve original on-disk ordering for byte-preserving repacks.

### 5.2 Type-specific payloads

| Type (`mode & 0xF000`) | Extra bytes following prefix | Path begins at `R+` | Notes |
|---|---|---|---|
| `0x4000` directory | No fixed extra fields | `0x18` | NUL-terminated relative path; `""` can represent root. |
| `0x8000` regular file | u32 data offset; u32 file byte length | `0x20` | Data offsets relative to **ImageFS body** `B`. |
| `0xA000` symbolic link | u16 `sym_offset`; u16 `sym_size` | `0x1C` | Symlink path string followed by target bytes; **the exact `sym_offset` base/off-by-one convention is disputed by public readers** and must be fixture-tested before write support. |
| `0x2000` character device | u32 device-related value; u32 rdev | `0x20` | Preserve raw values, avoid platform-specific `dev_t` assumptions. |
| `0x6000` block device | same | `0x20` | Same. |
| `0x1000` FIFO | same entry family | `0x20` | No regular-file contents. |
| `0x5000` QNX named-special | same entry family | `0x20` | QNX-specific; preserve even without host representation. |
| `0xC000` socket | same entry family | `0x20` | Preserve if seen; creation support requires validation. |

For a file, read exactly `file_length` bytes from `B+file_offset`. A zero-length regular file may have a convention-dependent offset; do not require it to point to allocated data, but require any nonempty file's range to be entirely within the ImageFS body **excluding the checksum**. Multiple entries can reference identical data ranges (deduplication/aliasing); do not insist on one unique extent per path.

For symlinks, let `P = R + 0x1C` (start of the NUL-terminated path field). The record stores two u16 fields `sym_offset` and `sym_size`, with the target bytes embedded later in the same record. **There is a genuine off-by-one disagreement in public readers:** the BSD-2-Clause `lclevy/dumpifs` extractor starts the target at `P + sym_offset + 1`, whereas the MIT `qnx_dumpers/ifsdump` displays it starting at `P + sym_offset`. This may reflect differences in whether the stored offset identifies the end-of-name NUL or the first target byte; neither reader alone establishes which layout QNX's `mkifs` emits for all profiles. A strict fsimg reader should locate and validate the pathname NUL, check both candidates against `sym_size` and the record boundary, and **treat ambiguous candidates as an unsupported or diagnostically ambiguous symlink instead of silently altering its target**. Do not pick a canonical writer convention until a real `mkifs` symlink fixture is checked with the official `dumpifs` and target `readlink`. The link target is content, not a file-data offset, and need not resolve to an in-image file.

Record path strings have **no leading slash**. Original bytes should be retained as bytes, even if a particular fixture is not valid UTF-8; a pretty display may decode with escaping. Because this is a flat path table, parent directory records may be implicit in certain images. A reader can materialize missing parents *logically* without inventing filesystem records.

### 5.3 Inode flag bits

When the ImageFS flags say inode high bits have meaning, the upper three bits have historically described **processed ELF**, **run-once ELF**, and **bootstrap executable** semantics:

| Mask | Interpretation for listing/round-trip |
|---|---|
| `0x80000000` | Processed ELF marker. |
| `0x40000000` | Run-once ELF marker. |
| `0x20000000` | Bootstrap executable marker. |

Keep the **complete original 32-bit inode word**. If normalizing the identifier, separate metadata bits from the remaining bits only for comparisons explicitly requiring it. `boot_ino[]` and `script_ino` in the image header refer to directory entries; record raw values, verify references to inodes under a defined matching convention. `0` means absent when used as an unpopulated bootstrap slot.

### 5.4 Optional extended attributes [V]

The common prefix contains an offset for optional extended data. Publicly observed material establishes that `0` denotes *none*, but does **not** establish a complete forward-compatible schema for all QNX releases. Proposed handling:

* Reader: check that nonzero offsets point **within the record**, preserve extension data verbatim and mark it opaque.
* Initial writer: set extension offset `0`; do not synthesize undocumented extensions.
* Repacker: preserve an unmodified original record exactly. If a file's name/record length changes, **refuse a lossless rewrite** rather than silently shifting an opaque structure that may contain internal offsets.

### 5.5 Ordering, padding and duplicates [V/P]

The record table is normally contiguous. Record lengths, rather than a guessed fixed alignment, determine iteration. A deterministic writer can choose 4-byte record padding and image padding, then ask `dumpifs`/`mount_ifs` whether the produced layout is accepted. The required minimum alignment for all QNX releases is **not** established by the documents reviewed; do not present “4 bytes” as an unconditionally required directory entry alignment.

Directory path order need not equal lexical order. A reader must preserve original order for reproducible repacking; a fresh writer can sort lexicographically, provided bootstrap/script inode links and file offsets are resolved after final layout. Duplicate names are ambiguous: reject in strict creation mode; include diagnostics and raw records in forensic mode.

## 6. Whole-image compression [D/O/V]

The startup header selects **one** compression method for the full ImageFS body. The startup section itself is not compressed. The official `mkifs` buildfile accepts `compress=1` zlib, `compress=2` LZO, `compress=3` UCL8; `[+compress]` selects UCL8. QNX 8.0 explicitly says **a zlib-compressed IFS is not bootable**, and notes the standard startup programs support UCL by default unless reconfigured.

| Method | Startup value | Observed encoding | Decoder/test approach |
|---|---|---|---|
| None | `0x00` | Raw ImageFS bytes begin at payload boundary | Parse `imagefs` directly. |
| ZLIB | `0x04` | Single zlib-family stream covering whole ImageFS | Use zlib library; verify wrapper/header against QNX-written fixture. |
| LZO | `0x08` | Sequence of length-prefixed compressed chunks | A BSD-3-Clause LZO1X decoder exists; test QNX chunk framing with official fixtures. |
| UCL8 | `0x0C` | Same chunk framing; UCL NRV2B-8 data | **Licensing gate:** upstream UCL is GPL; a BSD-only implementation needs a genuinely BSD-licensed NRV2B codec or independently implemented decoder. |

**LZO/UCL block representation:**

```text
repeat:
    2 bytes: compressed_payload_length in BIG-ENDIAN order
    N bytes: compressed block payload (N > 0)
until:
    2 bytes: 00 00, marking end of stream
```

A decompressed block is at most **64 KiB**. The **2-byte block length remains big-endian even when the surrounding target architecture is little-endian**. This is critical. No generic zlib/gzip encoder is interchangeable with UCL or LZO, and each block must be encoded in the correct algorithm/variant. QNX tool output should be used as the definitive writer acceptance oracle; third-party decoders are independent cross-checks.

For zlib, public implementations vary in their description between “libz/gzip” and `zlib.decompress` on the raw stored stream; exact wrapper flags need confirmation from actual `mkifs compress=1` output. Implement a strict verified profile once sample bytes exist, instead of trying every possible deflate interpretation and silently accepting an incorrect one.

**Resource controls:** limit decompressed bytes to `imagefs_size` (plus a small explicit diagnostic tolerance only in forensic mode); reject truncated chunks, zero-size nonterminal blocks, decompressor errors, overly large block outputs, premature EOF, unconsumed payload not explained by padding, and checksum mismatches. Do not allocate `imagefs_size` blindly without a caller-defined memory cap. A temporary file/backed seekable store is preferable for very large images because directory offsets provide random access *after* decompression.

**Third-party coverage caveat:** `lclevy/dumpifs` reads LZO; `qnxprobe` reads UCL, zlib and raw, but explicitly declines LZO and big-endian IFS pending real samples; `qnx_dumpers/ifsdump` advertises all four. Their shared success on one image is not proof of a universal parser.

**Compression licensing and provenance gate.** QNX's original `libucl` (NRV2B) and `liblzo2` are GPL-licensed; their code must **not be copied into a BSD-only fsimg**. [go-compressions/lzo](https://github.com/go-compressions/lzo) is an explicitly **BSD-3-Clause**, independently developed Go LZO1X decoder. It offers a one-block API suitable for adaptation to QNX's outer 2-byte chunk-length framing. Its implementation is **decoder-only**, not a QNX IFS writer, and compatibility with real QNX LZO blocks still needs testing. The [BSD-2-Clause `lclevy/dumpifs`](https://github.com/lclevy/dumpifs) parser **depends on `python-lzo`**, a wrapper around the LZO library; the BSD license on the Python parser does not relicense that runtime dependency. To honor a strict BSD-source-only mandate, use the BSD-3 decoder or independently license-audited replacement instead of vendoring GPL LZO code. The [naterator/routeros-extract](https://github.com/naterator/routeros-extract) project has an NRV2B decoder, but its own [license notice](https://github.com/naterator/routeros-extract/blob/main/LICENSE) explicitly says that particular decoder is adapted from **MIT-licensed RetDec** code; the repository-level BSD-3 license does **not** cover it. It is an external behavioral reference only, not eligible for copying under the user's rule. `qnxprobe` similarly declares MIT and requires separate provenance review for its UCL implementation. A compression *format description*, obtained from independent factual documentation and black-box tests, is not a license to reproduce GPL source. Using vendor-supplied QNX tools as a process-level oracle does not put their source in fsimg.

References: [QNX `mkifs` compression options](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkifs.html), [BSD `lclevy/dumpifs`](https://github.com/lclevy/dumpifs), [MIT `qnxprobe`](https://github.com/abrignoni/qnxprobe), [MIT `qnx_dumpers`](https://github.com/jtang613/qnx_dumpers).

## 7. Image checksum and image limits [O]

The final **four bytes inside `image_size`** hold the image checksum. For standard observed ImageFS images the checksum is the unsigned 32-bit value that makes the **sum of all 32-bit words across the entire ImageFS (including trailer) equal to zero modulo 2^32**, interpreted in the ImageFS byte order.

An original algorithmic definition (no source-code reproduction):

1. Construct ImageFS bytes with all final padding applied and reserve four trailer bytes.
2. Interpret the preceding bytes as endian-appropriate unsigned 32-bit words, summing modulo `2^32`.
3. Place the **additive inverse** of that sum in the four-byte trailer, using ImageFS endian.
4. Verify by summing all image words, including the trailer; the final sum must be zero.

Equivalent arithmetic: `trailer = (-sum(previous_words)) mod 2^32`.

These are the **ImageFS** checksum rules, not proof of startup-checksum or IFSCSA rules. The stated behavior is corroborated by an independently tested reader on real Ford SYNC G4 images, rather than merely assumed from a synthetic example. Full `image_size` includes the trailer. Do not confuse this additive checksum with CRC-32, CRC32C, POSIX `cksum`, MD5, SHA-256, or signed IFS authentication.

`image_size`, `hdr_dir_size`, `dir_offset`, file extents and directory-record sizes must all fit **32-bit ImageFS addressability**. A host process may use 64-bit arithmetic for checking sums and boundaries; **never** cast truncated host offsets into on-disk fields. Fail cleanly when computed IFS size would overflow the representable value.

References: [QNX `mkifs` output format](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkifs.html), [independent checksum measurements in `qnxprobe`](https://github.com/abrignoni/qnxprobe).

## 8. Path, metadata, executable and script semantics [D/O]

### 8.1 Paths and metadata

* On-disk path names are root-relative, separated by `/`, no leading slash; a root directory entry can be named `""`.
* Represent all path bytes losslessly in the parsed model. A user-facing extraction interface may provide canonical display and a separate raw-name escape mechanism.
* Preserve `mode` (including setuid/setgid/sticky), `uid`, `gid`, seconds-resolution `mtime`, inode flags, and any opaque record tail. Host filesystem permissions cannot always express QNX special nodes, so provide a **sidecar manifest** for fidelity.
* The filesystem is mounted under a configured mountpoint and uses a configured buildfile prefix. Do not make every entry's path start with `/proc/boot`: in an actual image `proc/boot/foo` may be the stored relative path.
* Keep hard links and symlinks distinct. `mkifs` can incorporate symbolic links explicitly and auto-create library SONAME symlinks. A duplicate data offset or inode value alone is not sufficient evidence to emit a host hard link; document aliases and verify QNX semantics with fixtures.

### 8.2 In-image executables and page alignment

`mkifs` may process ELF files, strip symbols, align executable text to page boundaries for **use-in-place (UIP / XIP)**, arrange copy-to-RAM data, and link special bootstrap programs. File data may therefore contain ELF images that do *not* match the host's source ELF bit-for-bit, and file alignment can matter to the loader. `fsimg` can extract/write regular bytes, but **should not claim a generated IFS will boot** until it also reproduces the target's required ELF processing, alignment, startup references, runtime address mapping and bootloader constraints.

An additional QNX-specific compressed-ELF representation may be embedded as file contents (separate from whole-IFS compression). Preserve such members byte-for-byte in the initial implementation. The MIT [`decompress-qnx-elf`](https://github.com/jtang613/qnx_dumpers) tool is a reference for diagnosing these *member files*; it is not an IFS writer.

### 8.3 Startup script (`.script`) is compiled binary, not necessarily text

`mkifs` consumes buildfile `+script` text directives at image build time and encodes executable boot-script instructions in a binary file, often named `.script`, referenced by `script_ino`. Runtime operations include launching programs, waiting for paths, reopening descriptors, showing text, and creating process-manager symlinks. Consequently:

* A normal `fsimg` reader/writer must treat `.script` as **ordinary opaque file bytes** by default.
* Do not extract the bytes, treat them as a shell script and then write them back as text; doing so changes the binary format.
* If script disassembly/editing becomes a requirement, develop a **separately gated codec with actual `mkifs` fixtures** and verify actions, scheduling options, argument counts and string alignment. Provide an informational text export, not a canonical round-trip representation, until all record classes are supported.
* Building **new bootable** images from a user-written boot script is better delegated to QNX's licensed `mkifs` until a tested independent script builder exists.

Official reference: [QNX boot scripts](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.building/topic/buildfiles/nto_script.html).

## 9. Chains, wrappers, OEM extensions and signed images [D/V]

1. **Chained filesystems:** the ImageFS header has a next-image physical address. It is not intrinsically a host-file offset. Resolve chains only with an explicit address-space map supplied by the loader/firmware container; otherwise show the address as metadata.
2. **Boot prefix:** optional machine instructions/data precede the startup header. `preboot_size` is a boot-time dimension, not always the length of a simple disk-file prefix; ELF/UEFI wrapping may introduce container relocation.
3. **ELF/UEFI wrappers:** QNX `mkifsf_elf`, `mkifsf_uefi` and other filters create loader-specific formats. A raw magic scan can identify an embedded candidate, but **rebuilding** a wrapped file requires container-aware segment sizing and checksums.
4. **S-record filtered output:** QNX explicitly says `dumpifs` does not directly handle e.g. `srec`-filtered output. Decode SREC into its address-space bytes first, preserving address records if writing back.
5. **Additional OEM containers:** Harman-Becker MIFS/HBCIFS and similar vendor formats can carry or precede ImageFS. Do not claim all such formats are standard QNX IFS.
6. **Digital signatures and trusted mount:** QNX `mount_ifs` supports signature verification and trust options (including `mount_ifs-auth`); a newly generated or byte-patched file is **not** automatically authorized for signed-boot environments.
7. **IFSCSA (QNX 8.0):** `mkifs` has `ifs_csa=crc32c` to insert an IFS checksum array **before** the startup header. This is distinct from the ImageFS trailer. The detailed array binary format was **not verified** from public material during this research. Treat as an opaque layer, preserving original bytes if possible, and do **not** synthesize CRC32C arrays until they can be independently validated with authorized 8.0 fixtures.
8. **Vendor integrity layers:** partition table checksums, update-package signatures, boot ROM checks and anti-rollback are outside `fsimg` ImageFS's format. A checksum-correct image can still be unbootable or rejected.

References: [QNX image filters and IFSCSA (`mkifs`)](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkifs.html), [QNX `mount_ifs` security](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mount_ifs.html), [QNX `dumpifs` SREC caveat](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/d/dumpifs.html).

## 10. Reader algorithm (proposed) [P]

### 10.1 Outer-container discovery

1. Receive an explicit format hint, or try standalone ImageFS magic at caller-provided offset.
2. If bootable detection is requested, search for **both endian representations** of the 4-byte startup signature within a caller-supplied bounded window (e.g., an IFS partition). Record candidate locations but do not treat a match as proof.
3. Validate startup header sizes, compression flags, numerical geometry, target-endian consistency and stored image range. Reject obvious false positives without reading huge segments.
4. For uncompressed images check `imagefs` at `startup_base + startup_size`. For compressed images decode within a resource budget, then check `imagefs` at the beginning of the decompressed output.
5. For a formatted container such as ELF, map file offsets through the appropriate segment table rather than assuming a physical address is a file offset. Expose `container_base`, `startup_base`, `stored_payload_base`, and `uncompressed_imagefs_base` as **different** concepts.
6. If more than one candidate passes validation, report multiple images/chains; do not silently select the first.

### 10.2 ImageFS parse

1. Read 88-byte fixed header prefix and infer endianness from its `flags`.
2. Validate sizes and NUL-terminated mountpoint within `dir_offset`.
3. Verify `image_size` and image checksum independently of path traversal.
4. Iterate records within `[dir_offset,hdr_dir_size)`, each with a checked size and type-specific minimum. Interpret `mode`, ownership, timestamp and inode metadata.
5. Validate file extents and symlink spans with overflow-safe arithmetic. Record each raw record and any opaque extensions.
6. Build a logical tree from relative paths, optionally synthesize *logical* missing parents. Note duplicate names and noncanonical paths without losing evidence.
7. Extract member contents using random access to decompressed ImageFS bytes, not directly from compressed offsets.

### 10.3 Proposed `fsimg` internal model

Keep a generic filesystem tree for normal library use and a **format-private IFS envelope** for fidelity:

| Model field | Purpose |
|---|---|
| `image_root`, `entries[]` | Path, kind, uid/gid, mode, mtime, data provider or symlink content. |
| `ifs.header` | Endian, flags, image size, directory bounds, mountpoint/mountflags, boot/script inodes, chain address. |
| `ifs.startup` | Optional startup header metadata, **raw 256 bytes**, **raw prefix**, **raw startup program/trailer**. |
| `ifs.compression` | None/zlib/LZO/UCL plus block/stream profile. |
| `entry.ifs_raw` | Original raw record, inode word, optional extattr/tail, original record index, exact name bytes. |
| `ifs.layout` | Original file offsets, alignments and opaque gaps, required for preserving image geometry. |
| `ifs.validation` | Independent check results with severity, positions and expected/actual values. |
| `ifs.security` | Presence/unknown markers for signatures, IFSCSA and OEM wrappers; no false assurance of trust. |

**Suggested behavior:** source image stays immutable; extraction may go to a filesystem, tar-like stream, or callback; portable metadata is returned through the library API even when host FS cannot express QNX nodes. Keep the IFS plugin independent from FAT/NTFS disk-block abstractions — ImageFS needs **contiguous random access to one immutable blob**, not block allocation.

## 11. Writer algorithm and practical boundaries [P/V]

### 11.1 Profile A: standalone/nonbootable ImageFS — do first

1. Accept a tree and optional metadata manifest (`mountpoint`, byte order, ownership, timestamps, special nodes, desired alignment).
2. Check path bytes for forbidden cases and host round-trip limitations; set absent UID/GID/mode/time by a documented policy, not implicit host-dependent defaults.
3. Assign stable inodes, reserving/remapping references for optional `boot_ino[]` and `script_ino` only if meaningful for that profile.
4. Calculate byte lengths of every variable directory record and its optional padding. Set extattr offset to zero for freshly generated records.
5. Choose data ordering (e.g. deterministic lexical order). **Lay out data offsets after completing directory sizing**, enforce selected alignment, and set `hdr_dir_size`, `dir_offset`.
6. Emit header, mountpoint, dir table, files and padding, then compute and append image checksum. Set `image_size` to total including the trailer.
7. Re-open with fsimg reader and official `dumpifs`, compare metadata and SHA-256 of extracted files. Where available mount on a QNX target via `mount_ifs`.
8. Keep the writer **deterministic**: same logical contents and metadata -> byte-identical output on the same options.

A nonbootable IFS is not “missing a required kernel”; it is a supported use case for QNX additional filesystem images. This decouples `fsimg` filesystem creation from firmware creation.

### 11.2 Profile B: bootable IFS that retains existing startup bytes

Accept a real source bootable IFS as a template. Keep its prefix, startup header and startup code **unchanged by default**. Generate a new ImageFS body separately. Only permit output if all necessary address/length/checksum corrections are **known and verified** for that exact profile:

* For equal-length, uncompressed replacements, stable offsets are simplest but image checksum still changes. Revalidate executable compatibility and signatures.
* For changed length, update `imagefs_size`, `stored_size`, relevant RAM sizes/physical address handling **as required by the specific loader**; do not mechanically set every size to the same number. Startup checksum may have to change if fields changed.
* For compressed data, reconstruct chunk framing or the selected zlib stream; ensure the loader actually implements the selected codec.
* For XIP/physical images, file alignment and absolute placement are potentially boot-critical.
* For signed/IFSCSA-equipped images, either implement and validate the full integrity layer using authorized materials or emit a **refusal to claim a bootable result**.

`fsimg` should offer a normal `create standalone-ifs` operation and a separately named `repack boot-container` experimental operation, with much stronger validation gates.

### 11.3 Profile C: create new bootable IFS independently — defer

Requires startup ELF/linked program, BSP-specific loader/boot prefix, executable linker processing, compiled startup script, address relocation, checksum rules and possible filtering/signing. The QNX SDP `mkifs` is a practical oracle and production writer where appropriately licensed. This goes beyond image-filesystem serialization.

## 12. Proposed command/API acceptance behavior [P]

`fsimg` names shown are **illustrative proposals**, not claims about the existing CLI:

```sh
fsimg -t ifs -l firmware.ifs                     # auto-detect bootable IFS, list
fsimg -t ifs -x firmware.ifs -C ./out           # extract and preserve sidecar metadata
fsimg -t ifs -c ./root -o standalone.ifs        # nonbootable raw ImageFS writer
fsimg -t ifs -V firmware.ifs                    # validate geometry, checksums
fsimg -t ifs -l disk.img --offset 0x400000     # inspect a known embedded image
```

Useful concepts for a library API: a `sniff` operation with candidate scores, a `parse` operation returning a filesystem tree plus optional raw metadata, a streaming `extract`, a `create` that accepts a content callback and a declared format profile, and `validate` that returns structured nonfatal and fatal diagnostics. Avoid conflating source-file byte offsets, target virtual addresses, and offsets inside decompressed ImageFS.

If the current fsimg CLI differs, the behavioral requirements above remain relevant; the flags are **not normative**.

---

## 13. Real read/write compatibility tools

### 13.1 Official QNX tools — primary oracle

| Tool | Reads | Writes | Where | Best use |
|---|---|---|---|---|
| [`dumpifs`](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/d/dumpifs.html) | **Yes**; list, verbose metadata, extract, decompress | No | QNX SDP host Linux/Windows and QNX 8 target | Gold-standard static validation. QNX 8 enforces license checks. |
| [`mkifs`](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkifs.html) | Consumes buildfile/source files | **Yes**, bootable and nonbootable | QNX SDP host, licensed | **Gold-standard generator** of positive fixtures and compression variants. |
| [`mkxfs`](https://qdn.qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.utilities/topic/m/mkxfs.html) | Buildfile/root dir | **Yes**, dispatch to IFS creator | SDP host | Frontend for several image formats; specify IFS type. |
| [`mount_ifs`](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mount_ifs.html) | **Yes**, mount into running system | No | **QNX target only** | End-to-end acceptance and content checks independent of `dumpifs`. |
| `mount_ifs-auth` (same manual) | Signature-verified mount | No | QNX target | Signed-image trust path; special keys and configuration needed. |
| `mkifsf_elf`, `mkifsf_uefi`, `mkifsf_srec` (documented under `mkifs`) | Wrap/transform | **Yes, container transformation** | SDP host | Wrapper fixture generation, not a substitute for core ImageFS creation. |

**Concrete read commands (QNX SDP):**

```sh
dumpifs -vv sample.ifs                       # list with additional header details
dumpifs -V listing.csv sample.ifs            # machine-readable CSV (supported 7.1/8.0)
dumpifs -m sample.ifs                        # per-file MD5 for file-content comparison
dumpifs -x -d extracted sample.ifs           # extract all files
dumpifs -f proc/boot/procnto -d one sample.ifs
dumpifs -u unpacked.ifs compressed.ifs        # save uncompressed copy
dumpifs -z sample.ifs                        # diagnostic ONLY: relax header sanity search
```

`dumpifs -z` turns off part of the header sanity checking; use it only for **investigation**, not conformance. QNX explicitly documents that SREC-filtered output is not accepted directly. QNX **8.0 `dumpifs` checks a valid QNX license before operating**.

The `-z` switch is useful when debugging a candidate that fails startup-header sanity checks, **not** a substitute for normal `dumpifs` acceptance. Record the success/failure of both default and relaxed modes when testing a new writer.

**QNX target acceptance:**

```sh
mount_ifs -f standalone.ifs -m /ifs-test
# Compare expected files inside /ifs-test with a manifest created from the source tree.
```

`mount_ifs` supports read-only mount (default), trusted mode, memory-address mounting, signed verification, and decompression threading (`-T`). For tests that involve auth, follow the target's security/ability requirements; do not use a failed auth mount as evidence the ordinary filesystem layout is invalid.

**QNX fixture creation:**

```sh
mkifs -nn test.build input-tree reference.ifs
```

This is the documented **three-position-argument form**: `buildfile`, optional root directory, output image. Use `-nn` to reduce host-timestamp nondeterminism. For a nonbootable buildfile, omit any bootfile/`virtual`/`physical` directive and supply ordinary filesystem entries. For compression permutations add the buildfile global attribute e.g. `[compress=1]`, `[compress=2]`, `[compress=3]` to otherwise equivalent fixture builds. As a new writer's oracle, compare semantic equivalence, not necessarily binary identity: `mkifs` may reorder, strip or align content.

**Version/host note:** official QNX **7.1** `mkifs` documentation lists Linux, **Mac**, and Windows hosts; the **8.0** page lists Linux and Windows. Do not assume the 8.0 tool is available on macOS. In all cases follow QNX licensing.

### 13.2 Open-source comparison tools (code licensing explicit)

| Project | License as currently published | IFS read/extract | IFS write | Caveat and recommended role |
|---|---|---|---|---|
| [`lclevy/dumpifs`](https://github.com/lclevy/dumpifs) — [`dumpifs.py`](https://github.com/lclevy/dumpifs/blob/main/dumpifs.py) | **BSD-2-Clause** | Yes, **LZO-focused** | **No** | **BSD-licensed IFS-format parser**; useful after preserving attribution. Its `python-lzo` dependency must be separately audited, and the parser is not a comprehensive codec/endianness oracle. |
| [`go-compressions/lzo`](https://github.com/go-compressions/lzo) — [`lzo1x.go`](https://github.com/go-compressions/lzo/blob/main/lzo1x.go) | **BSD-3-Clause** | **LZO1X blocks only**, not an IFS parser | **No** | Independently implemented LZO1X, suitable decoder building block for QNX LZO with separate QNX 2-byte framing and official fixture tests. |
| [`abrignoni/qnxprobe`](https://github.com/abrignoni/qnxprobe) — [`qnxprobe.py`](https://github.com/abrignoni/qnxprobe/blob/main/qnxprobe.py) | MIT | Yes, IFS from raw image/disk; UCL, zlib, none | No | Independent checksum and UCL comparison; declines LZO/big endian without fixture. **Do not copy source** per user's BSD-only rule. |
| [`jtang613/qnx_dumpers`](https://github.com/jtang613/qnx_dumpers) — [`ifsdump.c`](https://github.com/jtang613/qnx_dumpers/blob/master/ifsdump.c) | MIT stated in README; inspect individual files | Yes, advertises none/zlib/LZO/UCL | No | Cross-platform C differential reader; requires zlib, LZO and UCL development libs. **No code copying**. |
| [`askac/dumpifs`](https://github.com/askac/dumpifs) | License **not established** from repo; contains QNX-derived headers | Yes, various legacy formats | No | Historical behavioral comparison only; **do not use as a code source**. |
| [`harman-f/MHI2-MHI2Q-MH2P-Firmware-Toolkit`](https://github.com/harman-f/MHI2-MHI2Q-MH2P-Firmware-Toolkit) | **GPL-3.0-only** for project-authored material | Selected IFS inside OEM firmware | Explicitly **no general IFS rebuild** | Real firmware workflow/context; **do not copy code or documentation**. |
| [`ig0ist/SyncRenovation`](https://github.com/ig0ist/SyncRenovation) | Bundles third-party `mkifs` executables; redistribution rights **not established** | Ancillary | Wrapper invokes a `mkifs` binary | Shows what a vendor-specific rebuild workflow looks like, **not** a safely licensed independent writer. |
| [`vocho/openqnx` public mirror of `sys/image.h`](https://github.com/vocho/openqnx/blob/master/trunk/services/system/public/sys/image.h) | **QNX proprietary notice**; not BSD | Format-definition pointer | QNX build tools in tree | Link for provenance/reference, **do not copy header, structures or implementation**. |

**BSD-code reusability quick check:** `lclevy/dumpifs` (BSD-2) can provide IFS parsing inspiration/source, and `go-compressions/lzo` (BSD-3) can provide a reusable *LZO1X decoder* with its attribution. **Neither is a complete permissive UCL codec or a QNX ImageFS writer.** A BSD-3 project can still contain individual files under MIT or other licenses; check individual provenance before reusing a codec.

As far as this research established, **no validated BSD-licensed standalone IFS creator equivalent to `mkifs` was found**. That makes the standalone ImageFS writer a valuable and appropriately bounded `fsimg` contribution.

**Open-source CLI examples** (independent differential checks; use programs, not their non-BSD source):

```sh
# BSD-2-Clause reader: inspect its README for current Python dependencies/flags.
python3 dumpifs.py --help

# MIT qnxprobe from its documented CLI; enumerate IFS inside disk image:
python3 qnxprobe.py --list --depth 5 firmware-disk.img
python3 qnxprobe.py --extract files.zip firmware-disk.img

# MIT qnx_dumpers, on a Linux host with zlib/LZO/UCL development headers:
make ifsdump
./ifsdump -v image.ifs
./ifsdump -x -d extracted image.ifs
```

The repository projects are independent implementations with different coverage; failure of one reader does not necessarily prove the image is malformed. **The official QNX utilities and a real target mount are the acceptance oracle**.

### 13.3 Not IFS-compatible: common name collisions

* Linux `qnx6` kernel driver, `mkqnx6fs`, `mkqnx6fsimg`, and [Fox-IT `dissect.qnxfs`](https://github.com/fox-it/dissect.qnxfs) implement **QNX6 Power-Safe / QNX4** disk filesystems, not IFS. Fox-IT's code is AGPL-3.0.
* Netherlands Forensic Institute [`qnxmount`](https://github.com/NetherlandsForensicInstitute/qnxmount) is a QNX6/EFS-oriented project; do not expect it to be a general IFS validator.
* `mkefs` makes **EFS flash filesystems**, `mketfs` makes ETFS; they are not IFS writers.
* Executable extraction tools and `.ifs` wrapper filters operate at different container layers and do not replace an ImageFS encoder.

## 14. Fixture strategy and compatibility matrix [P]

Do **not** rely only on synthetic fixtures generated by `fsimg` itself. Maintain three sets: official `mkifs` golden images, third-party/real-world independently acquired legal fixtures, and handwritten/mutated invalid byte fixtures. Track SHA-256 of source images and record origin/license/expected metadata without embedding proprietary firmware blobs in the test repository.

### 14.1 Mandatory positive set

| Axis | Required cases | Reference tool |
|---|---|---|
| Container | Nonbootable raw ImageFS; bootable without prefix; bootable with prefix; embedded image at nonzero offset | `mkifs`, `dumpifs`; ELF wrapper later. |
| Endianness | Little; big | `mkifs [-bigendian/+bigendian]` when supported; `dumpifs`. |
| Compression | None; UCL; LZO; zlib | Four `mkifs` builds; verify decoders. |
| Sizes | 0, 1, 3, 4, 64 KiB-1, 64 KiB, 64 KiB+1, multiple 64 KiB, larger than 16 MiB | Extraction hashes and decompression boundary. |
| Names | Root; nested; long (>255 if accepted by builder); empty filename invalid; non-ASCII; semantically special names | `dumpifs -V`, target mount. |
| Nodes | Reg, dir, symlink (relative/absolute), chr, blk, FIFO, optional QNX named-special | `mkifs` supported `type` entries, target mount. |
| Metadata | UID/GID 0/nonzero, mode 000/755/4755, mtime 0/current, root permissions | `dumpifs -vv`, CSV or QNX `stat`. |
| Data placement | Empty files; unaligned tiny data; page-aligned executables; duplicate content; aliases | File hashes, byte offsets, `dumpifs`. |
| Boot fields | `boot_ino[]`, `.script`, physical/virtual image and differing `image_paddr` | Bootable official fixtures. |
| Version | QNX 6.5, 7.0/7.1, 8.0 if licensed fixtures available | Compare published variants, not guesses. |
| Security | Ordinary legacy checksum; 8.0 IFSCSA; signed mount | Gated fixtures, read-only validation first. |

The optional items are **not** implied to be supported on every version/platform. Record “unsupported by producer” separately from “fsimg parser failed.”

### 14.2 Writer acceptance protocol

For each independent fixture: `mkifs` → official `dumpifs -vv -V` → fsimg list → extract → compare normalized metadata manifest plus **SHA-256 of every regular file**. For each fsimg-produced image: fsimg create → fsimg parse+checksum → official `dumpifs` → QNX `mount_ifs` (when possible) → compare mounted tree. In no-compression mode, use binary offset inspections to distinguish format mistakes from metadata normalization choices.

Round-trip contracts should be separate:

1. **Semantic round trip:** every representable path/type/metadata/content agrees with source; raw bytes need not match.
2. **Byte-exact no-change repack:** when requested, a read-and-write of an untouched source must reproduce exactly the same bytes. **Only claim this if opaque wrapper/startup/padding can be preserved.**
3. **Bootability:** distinct end-to-end test requiring a QNX target/emulator and correct startup/loader. `dumpifs` acceptance alone is insufficient.

### 14.3 Suggested check script (illustrative, original)

```sh
set -eu
mkifs -nn minimal.build fixtures qnx-reference.ifs
dumpifs -vv qnx-reference.ifs > qnx-reference.list
dumpifs -V qnx-reference.csv qnx-reference.ifs
dumpifs -x -d qnx-files qnx-reference.ifs
fsimg -t ifs -l qnx-reference.ifs > fsimg-reference.list
fsimg -t ifs -x qnx-reference.ifs -C fsimg-files
# Compare normalized metadata manifests and SHA-256 trees here,
# not just a diff of filenames or a self-calculated checksum.
```

Adapt `fsimg` invocations to its actual CLI once IFS integration lands; shell example is a test-outline, not an already implemented driver.

## 15. Specification-derived invalid-case and fuzz plan [P]

This format is well suited to **field/range-derived fuzzing** (and ties in naturally to a generic spec-driven test framework): mostly scalar ranges, arithmetic relationships, variable records and nested bounds. Keep semantic “must reject safely” assertions separate from exact QNX error-message equivalence.

| Input or invariant | Legal boundary cases | Deliberately illegal/malformed examples | Expected fsimg reaction |
|---|---|---|---|
| Startup signature/version | LE/BE known magic, version 1 | Truncated magic, one-bit flip, false magic in file data | Fail detection or distinguish ambiguity. |
| `header_size` | 256 for standard | 0, 1, 255, 257, beyond remaining file | Reject unsupported header geometry. |
| `startup_size` | Header+startup+trailer, within stored | Less than header, overflows file, one off near EOF | Reject with precise bounds error. |
| `stored_size` | ≥ `startup_size`, bounded by container | Smaller, huge, wraps `base+size` | Reject without enormous allocation. |
| Compression enum | 0,4,8,12 | 0x1c, different flags mismatched body | Explicit unsupported codec or corrupt payload. |
| Block framing | N≤65535; decompression ≤65536; final 0 | Missing final 0; oversized decompression; EOF inside block | Error, no partial trusted extraction. |
| `imagefs` signature | Exact seven bytes | Corrupt byte, reversed magic used incorrectly | Reject standard format. |
| Header offsets | `0x58 ≤ dir_offset ≤ hdr_dir_size ≤ image_size-4` | Each `x±1`, negative-equivalent u32, overflows | Reject or diagnostic in forensic mode. |
| Mountpoint | NUL before directory | Missing NUL, NUL after directory, huge path | Reject as invalid header. |
| Directory length | ≥24 and ≤ remaining table | 0, 1, 23, entry crossing boundary, thousands of overlapping records | Exit boundedly, no infinite loop. |
| File byte range | Zero file, end at checksum start, shared range | file offset into out-of-bounds, start+len overflow, includes checksum | Reject unsafe extraction. |
| Symlink offset/len | Valid within record, relative and absolute targets | Link range outside record, unterminated pathname | Reject or preserve raw untrusted record. |
| `extattr_offset` | 0, valid nonzero within record | Offset beyond record, overlapping fixed fields unexpectedly | Preserve/report; never parse imaginary layout. |
| Types/modes | Reg, dir, symlink, device family | Unknown type, contradictory mode | Preserve raw if safe, don't invent contents. |
| Checksums | Correct sum, proper 4-byte padding | Flip one byte, truncated trailer, wrong endian sum | Structural invalid/corruption diagnostic. |
| Resource limits | Legitimate many entries and large data | Tiny file claiming 4GiB output, million records, decompression bomb | Reject with bounded memory/CPU. |
| Paths | Relative, `a/b`, non-UTF8 byte sequences | `/absolute`, `../escape`, embedded NUL in declared name | List safely; extraction path must not escape root. |

**Important extraction safety:** never follow symlinks while writing other extracted files; use `openat`-style jailed extraction or a safe equivalent; reject parent traversal and absolute stored names before writing to host; do not trust declared UID/GID to `chown` without explicit opt-in. Malformed image fuzzing should run under ASan/UBSan and memory/CPU limits. When comparing to `dumpifs`, recognize that vendor utilities may be less hardened against malicious input and isolate them from the fuzz corpus.

A minimal fuzz driver should treat a parse crash, out-of-bounds access, CPU timeout, undeclared memory growth, unsafe extraction path or falsely successful checksum verification as failures. Add a **writer property**: any successfully created ImageFS must be readable by fsimg and contain exactly the source tree; secondarily, it should be accepted by QNX tools for a supported producer profile. This property is practical and does not require SMT-solving the entire format.

## 16. Implementation stages for fsimg

| Stage | Capability / finish criterion | Risk |
|---|---|---|
| **A0** | Detect raw ImageFS/startup image, read header and bounds, report unsupported wrappers/codecs accurately. | Low. |
| **A1** | Read/extract uncompressed little-endian ImageFS, preserve metadata; checksum and malformed-case fuzzing. | Low. |
| **A2** | Full file types, symlink and root handling; big endian; `dumpifs` differential fixture suite. | Moderate (symlink/extattr corner cases). |
| **A3** | UCL8 compressed image reader; LZO then zlib readers, strict decompression limits. | Moderate; library/licensing choices. |
| **B1** | **Standalone nonbootable ImageFS creator**, little endian, file/dir/link, deterministic layout, checksum. `dumpifs` + `mount_ifs` accept. | Moderate, highest initial value. |
| **B2** | Big-endian and special-node writer, comprehensive metadata, directory alignment and inode-reference fixtures. | Moderate. |
| **C1** | Compressed ImageFS writer; compare semantics for all four QNX `mkifs` codec profiles. | Moderate/high. |
| **C2** | Experimental bootable-template repack, only for tested known-unsigned profiles, with size/checksum/physical-layout checks. | High. |
| **D** | Signed images, IFSCSA, boot script authoring, arbitrary EFI/ELF/OEM wrappers. | Very high; separate projects. |

**Recommended initial pull request:** A0+A1+A2 plus tests, followed closely by B1. `fsimg`'s distinctive value is already present when it can **create** a valid nonbootable ImageFS and **extract** bootable IFS, without attempting proprietary startup generation.

## 17. Open questions that require genuine fixtures [V]

These points should be tracked as explicit issues rather than silently replaced by assumptions:

1. Exact checksum procedure for startup header+code, including mutable startup-info region, padding and byte-order cases.
2. Which historic producer versions emit different startup-header tail variants, and whether an on-disk disambiguator exists beyond producer profile.
3. Precise zlib stream wrapper and end-marker/trailing-padding convention across SDP 6.x–8.0; 8.0 states zlib IFS cannot boot under standard startup.
4. Exact LZO/UCL encoder settings, maximum accepted chunk sizes, stream padding, whether empty images have special framing.
5. Symlink target-length terminator and directory record padding conventions in actual QNX `mkifs` output.
6. Presence and representation of extattr payloads across QNX versions and whether any fields have internal offsets.
7. Whether hard-link identity is represented by inode aliasing, data deduplication, an explicit mode, or buildfile-specific semantics for each producer version.
8. Precise independent validation of special devices, sockets, named-special files, and their two device-related words.
9. Alignment/physical-address invariants needed to retain XIP/processed ELF bootability after changing file order or size.
10. Portable handling of chained IFS when the next IFS is addressed through a physical memory map.
11. QNX 8 IFSCSA binary format, checksum coverage and validation/rebuild workflow.
12. Signed IFS authentication format and any upstream wrapper/bootloader signatures; out-of-scope for a generic raw writer without an authorized signing flow.
13. OEM-specific MIFS/HBCIFS, compressed-ELF members, ELF/UEFI/SREC boot wrappers and layouts. These are separate container adapters, not changes to core ImageFS.
14. Whether official QNX tools accept a standalone ImageFS with no explicit root record, non-UTF-8 pathname bytes, or nonstandard record sizes; testing should record precise producer/target version.

## 18. Reference catalog (direct links)

### 18.1 Authoritative public QNX documentation

1. [`mkifs` (QNX OS 8.0)](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkifs.html) — creator command, buildfiles, compression, image layout, IFSCSA, wrapping, version-specific features.
2. [`dumpifs` (QNX OS 8.0)](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/d/dumpifs.html) — complete reader commands, examples, CSV, license requirement, SREC caveat.
3. [`mount_ifs` / `mount_ifs-auth` (QNX OS 8.0)](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mount_ifs.html) — mount/verify operation, signed/trusted options.
4. [Startup header (QNX OS 8.0)](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.building/topic/ipl/ipl_startup_header.html) — byte-field purposes, address extension, flags.
5. [Startup header (QNX Neutrino 7.0)](https://get.qnx.com/developers/docs/7.0.0/com.qnx.doc.neutrino.building/topic/ipl/ipl_startup_header.html) — older header-tail layout.
6. [`mkifs` (QNX 7.1)](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.utilities/topic/m/mkifs.html) — historical compatibility and Mac host availability.
7. [`dumpifs` (QNX 7.1)](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.utilities/topic/d/dumpifs.html) — older oracle if 8.0 licensing not available.
8. [`mkifs` (QNX 6.5 SP1)](https://get.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/m/mkifs.html) — older image behavior.
9. [QNX boot-script semantics](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.building/topic/buildfiles/nto_script.html) — high-level QNX startup script syntax/semantics.
10. [QNX 7.1 OS images overview](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.building/topic/os_images/images_about.html) — boot images and `/proc/boot` concepts.
11. [QNX 8 filesystem architecture](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/fsys.html) — distinguish IFS from QNX6/EFS.
12. [`mkxfs` (QNX 7.1)](https://qdn.qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.utilities/topic/m/mkxfs.html) — unified host image builder.
13. [QNX support KB: “Mounting an IFS — Deep dive”](https://www.qnx.co.kr/support/knowledgebase.html?id=5011O000000xOgX) — landing page describes an attachment on checksum and mounting (access may require QNX credentials). **No proprietary attachment content included here.**
14. [QNX 6.3 System Builder image packaging](https://qnx.com/developers/docs/6.3.2/ide_en/user_guide/builder.html) — `.ifs` extension can represent more than a raw binary.

### 18.2 IFS-specific public code and test references — not copied

15. [**BSD-2-Clause** `lclevy/dumpifs`](https://github.com/lclevy/dumpifs) — Python LZO IFS reader/extractor; BSD license [here](https://github.com/lclevy/dumpifs/blob/main/LICENSE); [source](https://github.com/lclevy/dumpifs/blob/main/dumpifs.py).
16. [**MIT** `abrignoni/qnxprobe`](https://github.com/abrignoni/qnxprobe) — general disk-image forensic scanner incl. raw boot IFS, measured checksum and UCL decoder; [source](https://github.com/abrignoni/qnxprobe/blob/main/qnxprobe.py); **read/reference only** under BSD-only mandate.
17. [**MIT** `jtang613/qnx_dumpers`](https://github.com/jtang613/qnx_dumpers) — `ifsdump`, `efsdump`, compressed-ELF decoder; [IFS-specific source](https://github.com/jtang613/qnx_dumpers/blob/master/ifsdump.c); **read/reference only** under BSD-only mandate.
18. [**Unverified license** `askac/dumpifs`](https://github.com/askac/dumpifs) — legacy C extractor and QNX-derived headers; unsuitable code donor.
19. [**Proprietary** public `image.h` mirror](https://github.com/vocho/openqnx/blob/master/trunk/services/system/public/sys/image.h) — illustrates format provenance but not a legal source for BSD `fsimg` code.
20. [**GPL-3.0-only** Harman MHI2 toolkit](https://github.com/harman-f/MHI2-MHI2Q-MH2P-Firmware-Toolkit) — validated OEM data extraction and careful statement of IFS rebuild limitations; external context only.
21. [`ig0ist/SyncRenovation`](https://github.com/ig0ist/SyncRenovation) — OEM-specific IFS rebuild workflow relying on supplied `mkifs` binaries; do not import binaries with unclear redistribution licenses.
22. [Fox-IT `dissect.qnxfs`](https://github.com/fox-it/dissect.qnxfs) — **AGPL-3.0** QNX4/QNX6 parser, **not** IFS.
23. [NFI `qnxmount`](https://github.com/NetherlandsForensicInstitute/qnxmount) — **Apache-2.0** QNX6/EFS forensic tooling, **not** the IFS ImageFS writer.


24. [**BSD-3-Clause** `go-compressions/lzo`](https://github.com/go-compressions/lzo) — independent single-block LZO1X decoder, *not* an IFS parser/encoder; [license](https://github.com/go-compressions/lzo/blob/main/LICENSE), [source](https://github.com/go-compressions/lzo/blob/main/lzo1x.go).
25. [**BSD-3 project / MIT decoder** `naterator/routeros-extract`](https://github.com/naterator/routeros-extract) — NRV2B external oracle with mixed file provenance; its [license notice](https://github.com/naterator/routeros-extract/blob/main/LICENSE) confirms its NRV2B implementation is MIT-derived and **ineligible for code reuse here**.
26. [QNX-independent LZO stream-format prose reference](https://docs.kernel.org/staging/lzo.html) — technical decoding format reference; do not copy Linux GPL implementation code.
27. [UCL NRV2B codec licensing context (Debian source catalog)](https://sources.debian.org/src/ucl/) — GPL-licensed reference implementations; no source reuse here.

## 19. Final recommendation

Treat **ImageFS** as a compact static archive-with-POSIX-metadata embedded inside an optional QNX boot container. Make the ImageFS codec *independent* from the bootstrap/wrapper/compression adapters. The best implementation order is: **read uncompressed → handle metadata and checksum correctly → read UCL/LZO/zlib → create uncompressed standalone ImageFS → verify QNX target mount → consider bootable repacking**. Maintain a legally clean separation: BSD-licensed source may be reused subject to its license; all other linked implementations are for independent interoperability checks, **not copy/paste**.

The main unknowns for real firmware rebuilding are **not** the directory table: they are startup checksum/relocation, exact boot-time memory geometry, executable processing, new checksum arrays, and signatures. Keeping those outside the first writer milestone makes QNX IFS a practical addition to `fsimg` rather than an indefinite bootloader reverse-engineering project.
