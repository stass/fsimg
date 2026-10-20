# QNX4 filesystem: disk-format specification and fsimg implementation plan

**Edition:** Research draft 1.0 — 2026-10-19  
**Target:** fsimg image extraction, image creation, and structural validation.  
**Compatibility targets:** original QNX 4 filesystem and its QNX Neutrino `fs-qnx4.so` implementation; **not** the unrelated Power-Safe/QNX6 disk format.

> **Licensing/provenance rule.** This document is an original description of publicly documented on-disk facts, arithmetic, interoperability checks, and proposed fsimg behavior. It contains no copied code, proprietary source, or reproduced QNX documentation. Exact format identifiers, flag values, and field offsets are included only where necessary to interoperate. GPL-, AGPL-, MIT-, and proprietary implementations are identified for *external testing or comparison*, never as code to import into a BSD-only implementation. MIT is permissive but is not itself a BSD license; do not copy it under a strict BSD-only provenance requirement. Independently produce all fsimg code from this design and BSD-compatible sources, with review of applicable licenses.

## 0. Executive implementation summary

QNX4 is an extent-allocated, case-sensitive, POSIX-style filesystem. Its standard disk block is 512 bytes, its file/directory metadata record is 64 bytes, and an extent describes contiguous blocks. Ordinary filenames of at most 16 bytes share one record with file metadata. Longer filenames (17–48 bytes) occupy a link record whose file metadata resides in a special inode file. A compatible later extension stores names of at most 505 bytes in a dedicated special file. Bitmap bits describe allocation and are stored in a visible `/.bitmap` file. The layout has known starting blocks and a small set of mandatory special entries.

**Practical recommendation:** ship a read-only parser, validator, and a **legacy 48-byte-name writer** first. Produce a reference image with QNX `dinit`, compare it bytewise/semantically, and require both the independent Linux read-only driver and native QNX `chkfsys -f -u` to accept the result before advertising creation as interoperable. Long names over 48 bytes, hard links, special devices, bootable images, and recovery features should be staged in separately.

### Confidence notation

- **A — published and cross-checked:** confirmed by QNX documentation plus one or more independent on-disk layout descriptions; appropriate for parser implementation.
- **B — published/strong structural inference:** sensible to implement, but compare resulting image with QNX-generated fixtures before calling writer production-ready.
- **C — fixture required:** some exact byte value, initialization convention, or edge behavior is not established by the public references. Preserve while reading; don't invent values while writing.

Neither a synthetically generated filesystem nor successful mounting by a Linux parser is proof of full native-QNX compatibility.

## 1. Address space, sizes, and offsets [A]

All multibyte numeric fields described here are **little-endian**. Treat timestamps and block numbers as explicitly sized, never as host-native C structs. An inode size and extent count are not interchangeable: the former measures visible file bytes, the latter measures disk allocation.

| Quantity | Value / rule |
|---|---|
| QNX4 filesystem block | 512 bytes |
| Directory or inode slot | 64 bytes |
| Slots per block | 8 |
| Extent descriptor | 8 bytes: 32-bit starting block plus 32-bit block count |
| Extra extents in one extent-metadata block | Up to 60 |
| Bitmap coverage per 512-byte bitmap block | 4,096 filesystem blocks = 2 MiB of disk space |
| Filesystem block addressing | **1-based** inside the filesystem |
| Image physical byte offset for QNX block `b` | `partition_start_bytes + 512 * (b - 1)` |
| Typical public QNX4 file limit | `2^31 - 1` bytes |
| Original legacy filename limit | 48 bytes; directly embedded name up to 16 bytes |
| Long-filename extension | Up to 505 bytes if enabled |
| Filesystem size described by QNX docs | Up to `2^31 * 512` bytes (about 1 TiB), subject to disk-driver restrictions |
| Path length in QNX user guide | 1,024 bytes |
| UID/GID accepted by native QNX4 driver | Up to 65,534 |

`partition_start_bytes` is zero for a bare filesystem. For a partitioned whole-disk image it is the start LBA multiplied by the *outer disk logical sector size*. Do not confuse an MBR LBA with a QNX block number. It is best for fsimg's QNX4 backend to receive a byte-range view of one volume after partition-table discovery.

The values above distinguish on-disk field capacity from native API policy; e.g. a 32-bit file-size field exists but QNX's documented filesystem file limit is approximately 2 GiB minus one byte. Treat larger recorded sizes as suspicious/possibly non-portable rather than automatically valid just because they fit in 32 bits.

## 2. Volume layout and root metadata [A/B]

QNX 4's standard volume layout, in **QNX block numbers**, is:

```text
QNX block       Standard purpose
---------       ---------------------------------------------
1               Loader/bootstrap sector (512 bytes)
2               Root control block (512 bytes)
3 .. 2+B        Contiguous allocation bitmap, B blocks
3+B .. 6+B      Initially four blocks of root directory
thereafter      /.inodes, other metadata, file data, free space
```

Here `N` is the total 512-byte blocks and `B = ceil(N/4096)` bitmap blocks. QNX describes the four-block root-directory extent as the default `dinit` initialization. On an existing volume the *actual* root-directory extent must be read from the root inode, not computed as a permanent fixed address. The initial bitmap is known to follow the root block but the correct routine should still read `/.bitmap`'s extent metadata when validating it. [R1, R2]

### 2.1 Block #1: loader [A/C]

The first block is reserved for platform/boot-loader information. fsimg's data-only profile must reserve it and never treat it as a directory or data block. Actual QNX bootstrap bytes are platform-specific, may be copyrighted, and are out of scope. Whether all-zero loader bytes are accepted by a specific native QNX release must be checked; Linux's read-only parser does not rely on loader functionality. If a caller supplies a **lawfully obtained** loader blob, fsimg may accept it as opaque data and validate exact 512-byte length. Do **not** bundle proprietary boot code.

### 2.2 Block #2: root control block [A/C]

The first 256 bytes are four 64-byte inode-style records at these offsets relative to the start of QNX block 2:

| Offset | Length | Represents | Primary purpose |
|---|---:|---|---|
| `0x000` | 64 | `/` | Root directory inode; its name begins with the byte for slash `0x2F` |
| `0x040` | 64 | `/.inodes` | File holding off-directory inode records |
| `0x080` | 64 | `/.boot` | Optional primary boot image |
| `0x0C0` | 64 | `/.altboot` | Optional alternate boot image |
| `0x100..0x1FF` | 256 | Additional root-block information | QNX documentation mentions a label; exact byte-level layout is **not established here** |

The root inode's name is `/`. **There is no stand-alone QNX4 magic word** equivalent to an ext4 or QNX6 superblock signature: some Linux utilities report `0x002F` because slash appears at the start of this root record, but this alone is *not* a reliable format discriminator. For recognition, validate the root file type and extent bounds, traverse the root directory, and find a coherent `.bitmap` entry before claiming QNX4. [R3, R4]

**Writer:** zero or preserve unknown bytes in the second half according to an identified QNX reference profile; do not claim volume-label support until a `dinit -l` / `-L` differential fixture identifies the label position and terminator policy. Extracting a pre-existing image should preserve raw root-block bytes for diagnostics.

### 2.3 Root directory initially [A]

At formation, the root directory normally has four 512-byte blocks = 2,048 bytes = 32 slots. It includes `.`, `..`, `.bitmap`, `.inodes`, `.boot`, `.altboot`, and optionally `.longfilenames` (later format extension). The special file descriptors for `/`, `.inodes`, `.boot`, and `.altboot` are *root-block records*, even though entries bearing those filenames are also visible in the directory.

The root directory is otherwise an ordinary directory and can grow; its links are the exception: root `.` and root `..` refer back to the root inode in block 2. The precise special-entry encoding and initialization order should be captured from a native fixture before writing directories beyond a simplistic Linux-accepted test. [R1, R2]

### 2.4 Partitions and recognition [A]

Conventional MBR partition type bytes **77, 78, 79 decimal**, i.e. `0x4D`, `0x4E`, `0x4F`, designate QNX4-related partition types. They are hints; QNX filesystems are also used on raw volumes, and other filesystems may appear inside mismarked partitions. QNX6 Power-Safe commonly uses different type values (177–179 decimal) and must be routed to a separate parser. The current MBR at sector zero of an *outer disk* is not the loader sector of a QNX4 *partition*. Never accidentally overwrite a disk's actual MBR when creating a filesystem inside one partition. [R11]

## 3. Inode / direct directory-entry format: exactly 64 bytes [A]

All offsets below are within the 64-byte record, not within the disk sector. Field identifiers are descriptive labels created for this spec; they are not a pasted language-level struct.

| Offset | Bytes | Field (fsimg nomenclature) | Interpretation |
|---|---:|---|---|
| `0x00` | 16 | short_name | Raw filename bytes, zero-padded if shorter than 16; exactly 16 may have no NUL |
| `0x10` | 4 | file_size | Logical size in bytes, little-endian u32 |
| `0x14` | 4 | first_extent_block | First allocated QNX block number, u32, 1-based; zero for no data |
| `0x18` | 4 | first_extent_length | Number of 512-byte blocks in initial extent, u32 |
| `0x1C` | 4 | first_extent_list | QNX block of first overflow extent-metadata block; 0 when absent |
| `0x20` | 4 | creation_time | Seconds from 1970-01-01 UTC, u32 representation |
| `0x24` | 4 | modification_time | Last content write time, seconds since Unix epoch |
| `0x28` | 4 | access_time | Last read/access time, seconds since Unix epoch |
| `0x2C` | 4 | status_change_time | Metadata change time, seconds since Unix epoch |
| `0x30` | 2 | extent_count | Total number of extents in this file, including the first |
| `0x32` | 2 | unix_mode | 16-bit POSIX file type and permission bits |
| `0x34` | 2 | uid | Owner ID |
| `0x36` | 2 | gid | Group ID |
| `0x38` | 2 | hard_link_count | Number of names referring to this inode (interpret directory counts carefully) |
| `0x3A` | 4 | zero_or_reserved | Usually zero; must preserve on read; writer initializes to zero |
| `0x3E` | 1 | type_extension | Value/semantics not fully established; do not use as sole file-type source |
| `0x3F` | 1 | entry_status | Flags, including used/linked/other state |

This mapping is corroborated by the publicly documented QNX inode diagram and independent kernel layout descriptions. The 64-byte total is important: do not use native compiler padding. Bytes at offset `0x3E` and some possible status combinations need native fixture confirmation before writer semantics are declared stable. [R5, R6, R7]

### 3.1 Name encoding [A]

A short name occupies the first 16 bytes. If the name is 16 bytes long, **no trailing NUL is required**. Read its length as `min(first_zero_offset, 16)`. Never use an unbounded C string or `strlen` over a raw inode. Entry-status and directory context determine whether these 16 bytes are a name at all. Long directory entries use a different record format (section 4).

### 3.2 Mode, times, and owners [A/B]

The mode field is a 16-bit conventional Unix-style mode containing file-type bits and permissions. Useful file type values in octal: `0100000` regular; `0040000` directory; `0120000` symbolic link. It can also represent FIFO, character/block device, or other POSIX-style types; decoding their device payloads should be deferred until proven. QNX4 is case-sensitive and case-preserving. [R19]

QNX documents *four* timestamps, including a creation timestamp in addition to Unix atime/mtime/ctime. The disk stores whole seconds and no nanoseconds. On systems with differing 32-bit epoch-time semantics, dates outside the traditional 1970–2038 interval may be nonportable; preserve the raw unsigned integer and expose an interpreted date carefully. A formatter should select deterministic timestamps (or source metadata when requested).

Native QNX documentation limits numeric UID/GID to 65,534 even though fields have a representable maximum of 65,535. A portable writer should enforce the lower limit or explicitly provide a lossy/override policy. Hard-link count is 16 bits. [R12]

### 3.3 Status flags [A/B]

The high-level status byte is a bitfield. Known values are:

| Bit | Hex | Meaning observed in public format implementations / docs |
|---|---:|---|
| 0 | `0x01` | In-use/direct record |
| 1 | `0x02` | Modified-state flag |
| 2 | `0x04` | Busy-state flag (possibly interrupted allocation/update) |
| 3 | `0x08` | Directory link record, rather than direct inode |
| 4 | `0x10` | Inode-specific flag |
| 5 | `0x20` | Filesystem-clean state flag |
| 6–7 | `0x40`, `0x80` | Not standardized by the sources inspected; preserve, warn on unknown |

The two format-selection bits **must not** simply be treated as `status != 0`:

| `LINK` bit 3 | `USED` bit 0 | Directory slot interpretation |
|---:|---:|---|
| 0 | 0 | Unused/deleted slot |
| 0 | 1 | Direct inode record |
| 1 | 0 | Link record pointing to a separate inode |
| 1 | 1 | Invalid in documented layout |

Other bits may coexist; an older driver handles some heuristically. On corrupted images the parser should report the exact status byte. A `BUSY` record should be recognized as suspect rather than silently normalized. QNX's native driver has an explicit `unbusy` repair option, and `chkfsys` can examine it. [R1, R13]

## 4. Indirect directory link record: exactly 64 bytes [A/B]

For a name longer than 16 bytes, or for an inode with multiple hard links, QNX4 separates the filename from the inode. A directory slot stores a link record with **48 bytes of name**, a physical address of an inode slot, and flags.

| Offset | Bytes | Field | Interpretation |
|---|---:|---|---|
| `0x00` | 48 | name_prefix | Full name of length 17–48 bytes, or first 48 bytes of an extended name; zero-padded if shorter |
| `0x30` | 4 | referenced_inode_block | 1-based physical QNX block containing the target inode record |
| `0x34` | 1 | referenced_inode_index | Index 0–7 within that block |
| `0x35` | 4 | long_name_block | In extended variants, block address of a long-filename record; otherwise 0/unused [B] |
| `0x39` | 6 | reserved | Historical spare space; preserve when extracting or rewriting [C] |
| `0x3F` | 1 | entry_status | LINK set, USED clear according to documented slot-state rules |

The early Linux public header treats `0x35..0x3E` as ten spare bytes; newer QNX public documentation identifies the `0x35` pointer extension for names over 48 bytes. Interpreting old records with `long_name_block == 0` remains compatible. This is a **byte-level layout difference**, not a different 64-byte directory-entry size. [R5, R8]

### 4.1 How to resolve a link [A]

1. Confirm status marks a link (`LINK=1`, `USED=0`) and the directory name is valid.
2. Verify `referenced_inode_block` is in the volume and `referenced_inode_index < 8`.
3. Read `64` bytes starting at `block_offset(referenced_inode_block) + 64*referenced_inode_index`.
4. Validate that the destination is a plausible used inode record, generally located in the allocated extents of `/.inodes` for normal long-name/hard-link storage (with special-directory exceptions).
5. Interpret permissions, size, extents, and times from that inode, but take the visible pathname component from the *directory link*.
6. Keep a physical inode identity such as `(block,index)` to recognize hard-link aliases and avoid traversing the same object multiple times.
7. If long-name support is active and the link points to a plausible extended-name block, validate the back-reference and use the extended bytes as the effective filename; otherwise retain the 48-byte prefix and flag the missing extension.

**Important:** the referenced inode block is a **physical QNX block number**, not an ordinal into `/.inodes` or a byte offset. The `/.inodes` file is still useful to establish ownership/validity of the physical block.

### 4.2 Filename/hard-link representation [A/B]

- An ordinary file named `a.txt` with one hard link normally has metadata embedded directly in its parent directory slot.
- An ordinary file named `seventeen_char...` has its metadata in `/.inodes`, even if its hard-link count is one.
- Adding a hard link to a short-name file moves/shared-stores inode metadata in `/.inodes`, and both directory entries become link entries.
- Removing all but one name does **not necessarily re-embed** its inode. Existing indirect records with one remaining name are valid.
- The inode's internal 16-byte name field is **not** necessarily the actual pathname of each alias. Never substitute it for a directory entry's 48-byte name.

For fsimg writer v1, implementing long names (17–48) demands an inode allocator and correct physical back-pointers. Hard links can then reuse much of that infrastructure. [R2]

## 5. Extended names: `/.longfilenames` [A/B/C]

QNX Neutrino 6.2.1 introduced backward-compatible filenames up to **505 bytes**. The root directory must contain a special `/.longfilenames` file when this capability is enabled. Old consumers display the first 48 bytes; new native consumers can retrieve the full name. Some releases require filesystem restart after adding the feature file to an existing volume. The file can initially be an empty, read-only file with root ownership; populated volumes use it as a store of extended-name records. [R8, R9]

The published format illustration establishes the following record organization, each 512 bytes:

| Offset | Bytes | Field | Meaning |
|---|---:|---|---|
| `0x000` | 4 | owner_directory_block | Back-reference to the directory block containing the corresponding link |
| `0x004` | 1 | owner_slot_index | Directory slot index (0–7) within that block |
| `0x005` | 1 | extended_name_state | State/status byte; exact valid flag values **require native fixture** [C] |
| `0x006` | 505 | filename_bytes | Byte sequence for up to 505-byte full name |
| `0x1FF` | 1 | trailing_byte | Reserved/unspecified; do not ascribe meaning [C] |

A directory link stores its **first 48 name bytes** in the usual link format. A pointer at the link record's offset `0x35` leads to its extension block. The filename storage is in `/.longfilenames`, whose allocation is still described by ordinary extents. The 505-byte field may contain exactly 505 non-NUL bytes; do not require a terminator beyond the field. [R8]

**Nuances requiring fixtures:** whether `long_name_block` references a physical QNX block versus a logical record number; the complete status-byte lifecycle; reuse/deletion rules; whether one link can point to a record allocated beyond EOF of a zero-sized `.longfilenames`; and collision/alias semantics when two full names share the same first 48 bytes. A careful read-only implementation can parse plausible records with validation; creating >48-byte names must remain experimental until QNX creates and reads matching examples.

**`dinit -N` documentation discrepancy:** The QNX `dinit` help and user-guide pages say `-N` **prevents** the creation of long-filename support; a few `fs-qnx4.so` summary pages mistakenly say it enables it. Follow `dinit`'s own help, and check the actual binary in your environment. [R9, R10]

## 6. Extents and overflow extent blocks [A/B]

A file's data consists of an ordered sequence of **contiguous block runs**. The first run is in the inode at offsets `0x14..0x1B`. If the file has two or more runs, its overflow metadata starts at `first_extent_list` (offset `0x1C`) and consists of a chain of 512-byte extent blocks.

### 6.1 Extent semantics [A]

Each run has `(start, length)` of two LE u32 values. `start` is 1-based and `length` measures **blocks**, not bytes. To map a file-relative zero-based logical block number `k` to disk:

1. Visit runs in file order, first the inode's own run and then each run from successive extent blocks.
2. Subtract each skipped run's block count from `k` until it lies inside a run.
3. Its physical QNX block is `run.start + k_remaining` and its byte position is `partition_start + (physical_block-1)*512`.
4. Return only bytes up to the recorded file size; allocation beyond EOF is not user content.

A size-zero inode may have no extents. For a nonempty file require enough allocated run capacity to cover `ceil(file_size/512)` logical data blocks. Do not model QNX4 holes/sparse extents as zeroes without evidence of a supported on-disk convention. [R2, R7]

### 6.2 Overflow extent-block layout [A]

| Offset | Bytes | Field | Meaning |
|---|---:|---|---|
| `0x000` | 4 | next_extent_block | Physical QNX block of next extent-block, or zero |
| `0x004` | 4 | previous_extent_block | Previous extent block, zero for first [B] |
| `0x008` | 1 | entries_used | Number of extent descriptors used here, between 1 and 60 |
| `0x009` | 3 | reserved | Unspecified |
| `0x00C` | 4 | blocks_covered | Total data blocks described by *this block's* extent descriptors |
| `0x010` | 480 | entries | Exactly 60 eight-byte descriptors `(start,length)` |
| `0x1F0` | 8 | recovery_signature | Recognizable `IamXblk` prefix; remaining byte unknown/pad |
| `0x1F8` | 8 | first_extent_copy | Redundant copy of the inode's initial extent, documented especially for the first extent block |

**Interpretation:** the inode's first run is **not repeated as a normal entry** at `0x010`. A two-run file has `extent_count=2`, and its first extent-block contains one additional run. The first overflow block carries the recovery copy of run #1 at `0x1F8`. A writer can put the signature and first-run copy into every overflow block defensively, but that choice must be compared with native fixtures; reader should not demand the redundant copy match in all subsequent blocks. `blocks_covered` is not a global logical block count. [R1, R7]

### 6.3 Chain checks [A/B]

- The number of recovered descriptors must equal `extent_count - 1` for a well-formed inode.
- Each extent-block itself occupies one unique allocated QNX block, distinct from file data runs.
- `entries_used` must not exceed 60; zero with nonzero remaining extent count is invalid.
- Next and previous pointers, if both are nonzero, should agree.
- Reject loops, repeated extent metadata blocks, truncated chains, references outside volume, and extents crossing the end of volume.
- Sum the lengths of a block's used descriptors and compare to `blocks_covered` (strict on generated images; warning or failure in forensic extraction depending on observed native behavior).
- Compare the first block's stored first-extent copy to the inode's first extent, reporting a mismatch as a corruption/recovery clue.
- Reject data block aliasing between different inodes unless it is the exact *same inode* reached via hard links (same physical metadata identity). Do not implement copy-on-write semantics: QNX4 does not use them.
- Avoid integer overflow when testing `start + count - 1` or computing `count * 512`.

**Useful boundary:** one inode-embedded extent plus 60 in the first overflow block = **61 total extents**. The 61→62 transition is where the second extent-metadata block is necessary. Test these exact counts, as well as 1→2, 60→61, 61→62, 121→122.

## 7. Allocation bitmap and `/.bitmap` [A/B]

For volume block count `N`, logical bitmap size is `M = ceil(N/8)` bytes, needing `B = ceil(M/512) = ceil(N/4096)` physical 512-byte blocks. Bits are ordered **least-significant-bit first within each byte**; `bit 0` of the first byte corresponds to QNX block **1**, bit 1 to block 2, and so on.

**Meaning:** `1 = allocated`, `0 = free`. Out-of-range bits from `N` to `8*B*512-1` must be marked allocated (`1`) so the allocator never returns nonexistent blocks. [R1]

For a 10,240-block image:

- Bitmap requires `ceil(10240/4096) = 3` blocks, stored at QNX blocks 3, 4, 5.
- Root directory initially starts at QNX block 6 and occupies blocks 6–9.
- `/.bitmap` logical length is preferably `ceil(10240/8) = 1280` bytes; it allocates three physical sectors (1536 bytes). Unused trailing bytes/bits are all `1`.
- Root inode, bitmap, root directory, inode table, overflow blocks, long-name blocks, and content must all be represented as used in the bitmap.

The use of the **exact** logical bitmap length `ceil(N/8)`, rather than padded allocated capacity, is an fsimg writer recommendation based on Linux reporting filesystem-block totals as eight times the `/.bitmap` inode's recorded byte size. Confirm `.bitmap` EOF conventions against real `dinit` images of non-4096-aligned sizes before finalizing. [R7]

The root directory contains a **direct record** describing `.bitmap` and pointing to its physical bitmap extent. A parser should not infer a bitmap inode address from root block #2, because it is not one of its four special root metadata slots. A robust volume detector checks this record. On standard newly initialized volumes the bitmap is contiguous; on damaged/nonstandard volumes, examine inode data rather than silently assuming it is.

### 7.1 Allocation strategy proposed for fsimg

Because fsimg creates an image off-line, it need not reproduce QNX's first-fit allocation heuristics, allocation overrun, or crash-time ordering. Instead:

1. Reserve loader, root block, bitmap, four root directory blocks.
2. Reserve `.inodes` (typically 16 blocks for a hard disk), and reserve any enabled `.longfilenames` storage.
3. Precompute directory slots, inode-table requirements, and payload lengths.
4. Assign **large contiguous extents** per file and directory to minimize metadata complexity. Prefer one run/file.
5. Deliberately expose a test-only `fragmentation_pattern` allocator to force 2, 61, 62, and 122 runs.
6. Allocate overflow extent-blocks separately; build each file's chain.
7. Serialize file data and all structural metadata into a fixed-size output; write the bitmap last from the final allocation ledger.
8. Independently rescan the produced bytes with fsimg's reader and validator before making them available to callers.

This is a *writer design choice*, not a claim that native QNX uses the same placement algorithm. QNX documentation says its runtime allocator tries to extend the current extent and then seeks suitable free runs. [R2]

### 7.2 Special `/.bad_blks` [B/C]

The historical recovery guide refers to `/.bad_blks` for bad blocks. Its exact content and representation are outside this core spec. Reader should treat it as a normal system file unless explicitly implementing defective-media recovery. For generated virtual images there is no reason to synthesize bad blocks. [R1, R15]

## 8. Directories, special entries, and path traversal [A/B/C]

A directory's file payload is a concatenation of 64-byte slots. Directory size should be a multiple of 64, and typically an integer multiple of 512 in a generated image. Slots may be unused; no separate variable-length entry parser is required. The first two records are reserved by convention for `.` and `..`.

### 8.1 Directory-slot traversal

1. Read directory data through its own extents, never by assuming directory blocks are physically adjacent after the first run.
2. Examine all slots within the directory's *logical size*. Skip unused slots; report impossible `LINK+USED` combinations.
3. Obtain names either from the direct 16-byte prefix, link 48-byte prefix, or validated 505-byte extension.
4. Treat `.` and `..` specially for traversal; do not recurse into them.
5. Resolve metadata identity using direct physical `(directory_block, slot)` or referenced `(inode_block, slot)`; preserve hard-link identity for extraction.
6. Ensure a visited-directory set prevents cycles and repeated traversal of corrupted names.
7. For normal path lookup compare filename **bytes**, case-sensitively. Do not perform Unicode normalization or case folding.

### 8.2 `.` / `..` and embedded signature [A/C]

The first directory slot is the `.` link and includes a known directory identification byte sequence (ASCII `I`, byte `0x03`, then ASCII `QNX`) somewhere in its record. QNX's public guide establishes the byte sequence but does **not** specify its exact offset in the textual HTML, so this spec intentionally does **not** guess that offset. The `.` reference points to the entry describing the directory in its parent directory. The second (`..`) refers to the parent, documented in terms of the parent's initial directory block. Root `.` and root `..` both refer to the root inode in root block #2. [R1]

**Do not assume** all `.` and `..` records behave identically to arbitrary hard-link records. Their exact field-by-field payload, signatures, status bits, and the root/subdirectory cases are top-priority fixture questions. A Linux mount that can enumerate root files is not proof nested-directory creation is correct.

### 8.3 Root system entries [A/B]

| Filename | Nature | Required for minimal QNX4 data volume? |
|---|---|---|
| `/.bitmap` | Allocation bitmap represented as a file | Yes |
| `/.inodes` | File of extra inode records | Yes |
| `/.boot` | OS boot-image file; may be zero length | Conventional initial QNX volume: yes |
| `/.altboot` | Optional alternate boot-image file; may be zero length | Conventional initial QNX volume: yes |
| `/.longfilenames` | Long-name extension enable marker/storage file | Optional; enable only with tested extended-name behavior |
| `/.diskroot` | Optional disk-boot mount marker file | Not required for data-image MVP |
| `/.bad_blks` | Bad block handling | Optional; don't generate by default |

`/.inodes`, `/.boot`, and `/.altboot` metadata live in reserved slots in block #2. Their *directory entries* are aliases/references back to those slots. Conversely, `/.bitmap` is described in the root directory itself, not block #2. Validate this important distinction in the minimal generated fixture. [R1, R2]

## 9. The inode table: `/.inodes` [A/B]

`/.inodes` has file data consisting of 64-byte inode records. The standard native formatter allocates a minimum of one block for a floppy/RAM disk, and usually 16 blocks for other disks (native `dinit -i blocks` can select the initial count). The first slot is reserved and begins with the exact identifying ASCII bytes:

```text
IamTHE.inodeFILE
```

The constant is an interoperability signature, **not** sample application code. These 16 bytes occupy the slot's initial name field. fsimg must never allocate the first slot for a user inode. [R1, R10]

**Writer table allocator:** treat each remaining 64-byte physical slot in allocated `.inodes` blocks as capacity for one indirect inode; record a used/free occupancy map independent of whether a slot's internal name happens to be empty. A link record references the *physical QNX block containing* an allocated inode plus its within-block slot number. Expansion of `.inodes` itself can require updating its extent metadata. For v1, simplest is to **size `.inodes` up-front** to hold all long filenames and potential hard-link inodes, avoiding online expansion.

QNX does not require that every inode stay in `/.inodes`: inline direct records are the normal case. Conversely, a link-count-one file can legitimately retain an indirect inode after hard-link deletion. Extraction should preserve that object identity but need not materialize the internal `.inodes` file to the destination unless raw/forensic mode is enabled.

**Open item:** whether the native inode-table first slot's status, mode, timestamps, or other reserved bytes must be set to particular values in addition to the signature, and how secondary inode-table slots are flagged on allocation. Determine by comparing reference images. [C]

## 10. File data, symbolic links, and special types [A/B/C]

Regular files have uninterpreted byte streams in extents. **Symbolic link contents are pathname bytes stored as that special file's data**, not a Unix-like inline symlink embedded in the inode; QNX's public architecture describes a symbolic link as a special file whose content is the destination pathname. Detect via the inode mode. When extracting, create a real host symlink (when safe and requested) rather than a text file. [R17]

For symlinks, do **not** follow the pathname to decide which bytes to extract, and do not confuse hard-link status bit `0x08` with a symbolic link's POSIX file type. They are separate concepts.

A reader can safely surface permissions, owners, four timestamps, and mode type for other POSIX entries. Exact encoding of device major/minor numbers, FIFO behavior, and any native `type_extension` values is not verified; they should produce a rich metadata listing or a clear `unsupported-special` representation, not silently turn into regular files. Extended attributes, ACLs, and sparse-file encodings are not established by the sources checked and should not be advertised.

## 11. Recognition and parser security [A/B]

QNX4 has only weak first-byte signatures. A recognizer must be conservative, especially when fsimg scans arbitrarily embedded filesystems inside larger firmware images.

Recommended recognition phases:

1. Check sufficient bytes for loader plus root block (at least 1,024 from candidate partition start).
2. Parse the four root-control records at QNX block #2; require first name `/` and a plausible directory `mode`.
3. Require root-directory first extent to point within bounds and to contain enough space to read a directory slot.
4. Read root directory and locate a coherent `.bitmap` record. Validate its name, `USED` status, mode, extent bounds, and expected bitmap byte length/coverage as appropriate.
5. Optionally locate `/.inodes` and verify `IamTHE.inodeFILE` at its first allocated block; this strengthens confidence but forensic reading should allow a missing/damaged file as a warning.
6. Confirm bitmap marks at least the loader, root block, bitmap storage, root-directory storage, and inode-table storage in use; treat a mismatch as corrupt/untrusted rather than automatically rejecting possible recoverable volumes.
7. Do **not** classify a filesystem based solely on an MBR type or the slash byte at image offset 512. Distinguish QNX6 explicitly.

Keep arithmetic in 64-bit checked form. A malicious image can claim a 2 GiB directory, millions of extents, circular link resolution, overlapping bitmap storage, inconsistent `di_size`, or directory entries pointing to the loader/root sector. Enforce configurable maximum metadata reads, maximum path depth, visited sets, and global per-volume traversal budgets. Prefer error objects (`invalid extent`, `cycle`, `short image`, `bad signature`) over a generic I/O error.

Raw mode may report damaged records with confidence and byte locations; normal extraction mode should refuse unsafe reads and should never make files outside the requested destination directory. Sanitize absolute paths, `..`, embedded NULs, reserved host names, and symlinks during extraction.

## 12. Writer profile: proposed normative fsimg behavior

The following rules are **proposed**, not alleged vendor formatter rules. They can become a production profile once validated against native QNX fixtures.

### 12.1 `qnx4-legacy` profile (first stable release)

- Fixed 512-byte filesystem blocks; unpartitioned volume; nonbootable data image.
- Image size a multiple of 512; configurable minimum; reject volumes too small for fixed metadata.
- Start with QNX volume block 1 loader, block 2 root control, contiguous bitmap, four-block root directory.
- Reserve `.inodes` (16 blocks, except explicit tiny-volume test profile); initialize mandatory signature in first slot.
- Create root metadata entries and `.bitmap`, `.inodes`, `.boot`, `.altboot` conventional names.
- Populate directories with `.` and `..`, but enable subdirectories **only after** their special-entry fields have been checked with native fixtures.
- Names of 1–16 bytes: direct inode. Names of 17–48 bytes: link record plus separately allocated inode.
- Regular files, empty files, directories, and symlinks in priority order; hardlinks when inode-table behavior is validated.
- Extents contiguous by default; support multi-extent writer when extent-chain fixtures are validated.
- No `.longfilenames` file by default in this legacy profile; cap names at 48 bytes; do not truncate silently.
- Reserve and mark all physical blocks used; fill unused bitmap/out-of-range bits with ones.
- Strictly prevent duplicate case-sensitive names, invalid filename bytes, overlong paths, and unsupported special types.
- Nonbootable loader sector either left as profile-defined zero or supplied by user; make no promises of bootability.
- If any incompatibility/unknown bytes are not yet known, the backend should return `experimental_compatibility` rather than falsely claiming all-QNX interoperability.

### 12.2 `qnx4-extended` profile (future)

- Create the `/.longfilenames` feature file and allocate per-long-name records; support names 49–505 bytes.
- Maintain precise owner directory block/index back-references and link pointers.
- Generate deterministic truncated 48-byte prefixes consistent with native QNX; reject collisions until native aliasing behavior is known.
- Add hard links and deep directory trees, perhaps boot-image insertion if legally supplied.
- Validate with **native QNX6.x `fs-qnx4.so`**, not only Linux's legacy QNX4 driver.

### 12.3 Why not implement in-place mutation?

An off-line creator can fully plan allocation then serialize the finished image. Native QNX4 attempts to preserve consistency across interrupted writes by forcing sensitive metadata changes to disk in an intentional order, with busy/clean indicators. Reproducing crash-consistent in-place writes would require a separate transaction/update ordering specification and extensive power-cut testing. It is unnecessary for fsimg's create/extract model. Avoid modifying already-mounted images and avoid advertising a general read-write filesystem mount implementation. [R2, R13]

### 12.4 Suggested backend decomposition

This is an original proposed implementation architecture, not copied source:

| Component | Responsibility |
|---|---|
| `BlockView` | Bounded 512-byte volume-relative reads, offset arithmetic, LE field decoding |
| `RootLayout` | Root inode slots, special files, canonical/actual layout checks |
| `Inode64` | Decode/encode 64-byte inline inode records without struct casts |
| `DirLink64` | Decode/encode indirect directory links and optional long-name pointer |
| `ExtentMap` | Logical→physical data iteration, chain tracking, redundant-copy validation |
| `Bitmap` | Allocation state, reserved metadata accounting, expected-versus-actual ledger |
| `InodeTable` | Allocation and resolution of `.inodes` slots, hard-link identity |
| `LongNames` | 505-byte extension records (optional, gated) |
| `DirectoryWalk` | Safe listing, path lookup, cycle control, metadata interpretation |
| `ImagePlanner` | Two-pass or multi-pass assignment of all directory slots, inode slots, payload extents |
| `QNX4Verifier` | Ownership checks, corruption diagnostics, checker report, policy modes |
| `CompatibilityHarness` | External Linux/QNX qnxprobe/chkfsys comparison, golden manifest |

Keep decoding and serialization independent from the host filesystem implementation. That makes it straightforward to fuzz raw structures, extraction, and creation separately.

## 13. Validation / consistency checker specification

A useful fsimg QNX4 checker is more than a mountability test. It should reconstruct a **block-ownership graph**:

### 13.1 Allocation ledger

- Seed owned blocks: loader, root control, bitmap, root-directory blocks.
- Discover inode-table extents, overflow extent blocks, optional longfilename blocks, every file's data runs and directory runs.
- For every owned block, record **type and owner** (`root`, `bitmap`, `inode_table`, `dir:...`, `data:inode...`, `xblk:inode...`).
- Compare actual reachable allocation with `/.bitmap` bits; distinguish **missing allocation** (reachable but marked free) from **orphan allocation** (marked allocated but not reachable).
- Recognize *hardlink names* to the same metadata identity, so multiple pathnames do not make file data look cross-linked.
- Flag two different inode identities owning the same physical extent, xblk/data overlap, cyclic extent chain, and mandatory metadata left free.
- Validate trailing bitmap bits are allocated even if they represent no disk blocks.

### 13.2 Structural invariants

**Fatal or high-severity:** a bad root inode, out-of-range reference, integer overflow, physical read beyond image, unsupported record status, directory cycle not attributable to `.`/`..`, metadata block used as payload, extent chain length discrepancy, or missing required bitmap.

**Important inconsistency:** allocation-bitmap mismatches, inode hard-link count disagrees with count of referencing names, directory `.`/`..` points to incorrect object, extent recovery signature missing, incorrect block count in xblk, `.inodes` first-slot signature missing, long-name back-reference invalid, directory size not multiple of slot size, or duplicate names.

**Warning:** unknown inode type byte, unknown status bits, timestamp out of portable range, trailing garbage in reserved bytes, an unused inode-table record holding nonzero bytes, a noncanonical but valid directory placement, root label with unknown encoding, or file allocation beyond EOF. Some historical native QNX volumes will legitimately retain fragments/overalloc; do not discard them automatically.

### 13.3 Modes and reporting

- **strict:** require an internally coherent, canonical image suitable as writer acceptance criterion.
- **compatible:** accept noncanonical yet navigable native variations; report warnings.
- **forensic:** salvage whatever can be read, annotate field offsets and confidence; do not silently fabricate file bytes.

Export machine-readable findings containing a normalized code, QNX block number, record byte offset, affected path/inode ID, severity, and comparison of expected/observed values. No automated repair in the initial implementation.

## 14. Test tooling: actual read and write capabilities

**Do not conflate QNX4 and QNX6.** `mkqnx6fs` and `chkqnx6fs` work on Power-Safe filesystems and are **not** creator/checker tools for this format.

| Tool | Platform | Read | Write or create | Role / constraints | License |
|---|---|---:|---:|---|---|
| **QNX `dinit`** | QNX Neutrino; some releases also Linux and Windows | No | **Yes — format** | Canonical native QNX4 initializer; `-S`, `-i`, `-N` documented; use legal installation | Proprietary QNX binaries |
| **QNX `fs-qnx4.so`** | QNX Neutrino | **Yes** | **Yes** | Native filesystem driver; authoritative round-trip oracle | Proprietary QNX |
| **QNX `chkfsys`** | QNX Neutrino / QNX4 | **Yes, inspect** | Can repair | Best validation oracle, use `-f -u` to force nonrepair scan | Proprietary QNX |
| **Linux `qnx4` filesystem** | Linux with `CONFIG_QNX4FS_FS` | **Yes** | **No** | Good independent kernel read, legacy names <=48; read-only even if mounted without `ro` | GPL-2.0-only |
| **FsRover** | Windows/Linux | **Yes** | **No** | Multi-filesystem explorer/extract, lists QNX4 and QNX6 | GPL-3.0-or-later |
| **qnxprobe** | Python/Linux/macOS/Windows | **Yes** | **No** | Standalone image/partition detection, listing and ZIP export; self-test creates synthetic images; native QNX fixture still missing | MIT (not BSD) |
| **Dissect `dissect.qnxfs`** | Python | **Yes** | **No** | Forensic read/parser for QNX4 and QNX6 | AGPL-3.0 |
| **`qnx-modules-dkms`** | Linux (DKMS) | Via kernel | No | Build/install QNX4/6 kernel drivers if distro didn't provide; kernel-version maintenance risk | GPL-3.0 in repo packaging |
| **Peter Waechtler Linux-side formatter** | Historical Linux utility | No | **Yes — format (historical)** | `dinit`-like program linked from Anders Larsen's QNX4fs page; availability/operability/license **not independently audited** | License unverified; not reusable |
| **Historical alternative Linux QNX4 driver** | Legacy Linux patch | Possibly | **Experimental write** | Linked as an unmaintained read-write fork; **not** a reliable oracle | License/support unverified |
| **`dd`, `losetup`, `sfdisk`, `qemu-img`** | Linux/Unix | Container-level | Container-level | Prepare, wrap, inspect, and snapshot images; not QNX4 filesystem implementations | Various; no source inclusion |

There is no commonly supplied modern Linux `mkfs.qnx4` or `fsck.qnx4` utility on par with `mkfs.fat`; the Linux QNX4 driver is read-only. The native QNX tools are the most meaningful *write* oracle. Some standalone parsers are newer than the older kernel implementation, but synthetic self-tests are not independent native-QNX validation. [R10–R16]

### 14.1 QNX formatter to create a golden volume

The QNX 6.5/6.6 `dinit` documentation supports allocating/growing an ordinary output file with `-S`, using `k`, `m`, or `g` suffixes. A candidate **disposable-file** command (check the actual version's `use dinit` output before running):

```sh
# RUN ONLY AGAINST A DISPOSABLE FILE, never a physical drive:
dinit -S 16m ./golden-qnx4.img

# Legacy 48-byte filename profile, if this version supports -N:
dinit -N -S 16m ./golden-qnx4-48.img

# Optional test of .inodes size, exact support depends on version:
dinit -i 16 -S 16m ./golden-qnx4-16inodes.img
```

If direct file formatting is unsupported in the installed build or QNX's mount path requires a block device, use an emulated QNX-supported disk with a disposable MBR QNX4 partition and run `dinit -h` on that **specific partition device**, never on the host disk. The QNX 6.5 manual explicitly describes `-S` file growth and `-N` disable-longnames. QNX4.x older `dinit` has a different flag set, so do not assume the options transfer backward. [R10]

### 14.2 Native QNX driver and consistency checker

Inside an isolated QNX guest with a disposable partition (names are examples):

```sh
mount -t qnx4 /dev/hd0t77 /mnt/qnx4
# Create a test corpus with mkdir, cp, ln, ln -s, chmod, chown, touch, etc.
sync
umount /mnt/qnx4

# -f = no fixes, -u = ignore clean flag, -m = raw device on Neutrino:
chkfsys -f -u -m /dev/hd0t77
```

Always use a snapshot or a copy. Even a native driver may alter metadata simply by mounting read-write (e.g. access time or clean/busy state), so preserve the before/after disk-image bytes for comparison. `chkfsys` without `-u` may see a clean flag and **skip examination**; without `-f` it can offer repairs or change the disk. Ensure no writer has the filesystem open while testing. [R14, R15]

### 14.3 Linux read-only mount and extraction

```sh
# Linux VM; requires a kernel containing/building CONFIG_QNX4FS_FS
sudo modprobe qnx4
sudo mkdir -p /mnt/qnx4
sudo mount -t qnx4 -o loop,ro ./candidate.qnx4 /mnt/qnx4
find /mnt/qnx4 -xdev -print
# Compare manifests, symlinks, owners, modes, hashes, and file bytes.
sudo umount /mnt/qnx4
```

This assumes an **unpartitioned filesystem image**. For whole-disk MBR images, expose the partition first using `losetup --find --show --partscan ...` or mount with an explicitly calculated offset; *never* let the guest mount the host disk inadvertently. Some distributions compile QNX4 out of the kernel; installing util-linux does not supply the filesystem driver. The Linux driver is always read-only and may not fully expose 505-byte names. [R7]

### 14.4 Standalone cross-platform readers

`qnxprobe` can list and export filesystem content without privileged mount access; it is especially valuable in CI. Its README documents flags:

```sh
python3 qnxprobe.py ./candidate.qnx4 --list --depth 8
python3 qnxprobe.py ./candidate.qnx4 --extract ./extracted.zip
python3 qnxprobe.py --self-test
```

The flags and general invocation are taken from the tool's public README; check `--help` for exact argument ordering of the version you use. Its author describes a synthetic QNX4 fixture containing nested directories, a fragmented file, a long (legacy <=48-byte) name, a symlink, empty file, permissions, owners and timestamps, whose extracted content and metadata agreed with a Linux qnx4 mount. The README explicitly says the corpus lacks a confirmed real/native QNX4 source image: treat it as valuable secondary, **not** final, evidence. [R16]

FsRover includes a QNX4 reader and can be used to list/extract raw or partitioned disk images without QNX; Linux `LinuxRover --file=image --list` and `--mount=img0 ...` examples are supplied in its README. Dissect exposes a Python `QNX4(fh)` reader API and per-inode `listdir`, `open`, `dataruns`, metadata methods; independently compare outputs of all three. Their GPL/AGPL licenses prohibit copying implementation code into a BSD-only source tree. [R20, R21]

### 14.5 Historical Linux-side formatter

Anders Larsen (former maintainer of the Linux QNX4 driver) links a **`dinit.tgz`** archive of Peter Waechtler's old Linux-side QNX4 filesystem initialization utility. Useful for archaeological comparisons and perhaps as a third *writer* oracle if its old build can run in a disposable VM. Its provenance, license, trustworthiness, and current link availability need manual auditing. Do not embed, redistribute, or import its source into fsimg without legal/quality review. It is not a substitute for native-QNX `dinit`. [R18]

## 15. Differential and property-based test program

### 15.1 Two-way oracle strategy

For each fixture, create a **golden manifest** of pathname bytes, node type, file size, SHA-256, uid/gid, mode, timestamp seconds, hard-link identity, symlink target, and per-file logical→physical extent list (the last item may require fsimg's verifier). Compare:

1. **QNX-origin:** native QNX `dinit` plus native filesystem writes → fsimg extraction and validation → independent Linux/qnxprobe reads.
2. **fsimg-origin:** fsimg create → fsimg extract and validate → Linux QNX4 mount and qnxprobe extract → native QNX mount, open, append, rename, `chkfsys -f -u` (on a copy).
3. **Mutation-origin (negative):** copy a known-good image; change exactly one disk field; prove fsimg reports correct corruption code, no crash/hang/OOB, and no writes occur.

Treat Linux + qnxprobe agreement as **one weak family of oracles**, because qnxprobe's QNX4 format information is related to the Linux driver. Native QNX adds the crucial independently implemented writer/checker.

### 15.2 Format test matrix

| Dimension | Boundary cases |
|---|---|
| Volume blocks | Smallest viable, 1,024, 4,095, 4,096, 4,097, 8,191, 8,192, 8,193, nonmultiples of 8; image sizes large enough for 1,2,3 bitmap blocks |
| Name length (bytes) | 1, 15, **16, 17**, 47, **48, 49**, 504, **505, 506**; reject 0 / >limit |
| Filename bytes | ASCII; UTF-8 multibyte where supported; invalid byte `0x00–0x1F`, `0x7F`, `0xFF`, slash; names differing only by case |
| Regular file length | **0, 1, 511, 512, 513**, 1 KiB, exact extent capacity, one byte beyond |
| Number of data extents | **0, 1, 2, 60, 61, 62, 121, 122** |
| Directory occupancy | Empty except `.`/`..`; final available slot; first overflow block; many children; nested directories |
| Inode slot occupancy | First reserved slot, last slot in `.inodes` block, first slot of next block, table needs growth |
| Link identity | One inline file, 17-byte-name indirect, 2 hardlinks, 3 hardlinks across directories, removal leaves 1 indirect |
| Symbolic links | Relative target, absolute target, dangling target, symlink to directory, symlink cycle, empty target if native permits |
| Metadata | uid/gid 0 and 65534; reject/handle 65535; mode 0000/0644/0755; all four timestamps including seconds 0 and 2038 boundary |
| Bitmap | 1 vs 2 bitmap block boundary; beyond-EOF high bits; missing allocation; orphan allocation; double-owned block |
| Extent errors | Zero start with positive length, overflow, out of range, loop, bad previous pointer, bad signature, inconsistent count |
| Directory errors | Bad status bit combination, nonterminated 16/48 name, invalid inode pointer/index, dot signature mutation, cycle |
| Root/special files | Missing bitmap, missing inode signature, root inode corruption, longnames marker absent/present, unknown label bytes |
| Containers | Bare image, MBR QNX4 partition, false-positive `0x4D` with FAT/garbage, QNX6 image, truncated image |

**Prioritize** the transition from short to indirect names (16→17), the extent chain creation boundary (1→2 and 61→62), and bitmap-block growth (4096→4097). These are the highest-leverage structural bugs.

### 15.3 Fuzzer targets

- `decode_inode64(bytes[64])`: deterministic structured field parser and validator.
- `decode_link64(bytes[64])`, `decode_xblk512(bytes[512])`, `decode_longname512(bytes[512])`.
- `mount_like_reader(image_bytes)`: limit global blocks/path nodes/depth to prevent fuzz-induced excessive allocations.
- `extract_image(mutated_image)`: assert no writes outside destination and never follows symlinks out of it.
- `create(external_manifest)`: random legal trees, names, sizes, times, hardlinks, symlinks, allocation fragmentation.
- `roundtrip(manifest)`: image → parse → normalized manifest; assert equality except explicitly unsupported/lossy metadata.
- `bitmap_model(image)`: property checker recomputes reachable allocations and verifies bitmap state.

Use structured mutation operators: flip a bit in each slot's status, change exactly one extent start/length, corrupt `IamXblk`, redirect an inode index, change size across an allocation boundary, remove `.bitmap`, reverse one bitmap bit, introduce cycle, redirect longname owner, zero one root field. Pure random byte mutation alone will spend most cycles failing recognition.

### 15.4 Controlled source corpus policy

A strong corpus should contain: (a) QNX4-created volumes **without redistribution of QNX binaries**, (b) synthesized fsimg images, (c) audited synthetic images from independently licensed test tools if their fixture licenses permit use, and (d) controlled mutated variants. Check QNX licensing before publishing native-created binary fixtures, especially if they contain proprietary loader bytes or OS images. You can publish checksums/manifests and fixture-generation *instructions* separately if redistributing images is restricted.

For golden fixtures, record QNX version, `dinit` command, mount driver version, partition device geometry, `chkfsys` output, original sha256, extraction sha256, and whether `.longfilenames` was enabled. Fixtures need versioned provenance, not just `.img` files in a directory.

## 16. Known uncertainties and prioritized experiments

The public documentation is unusually good on the main layout, but precise **writer compatibility** still has gaps. Do not hide them behind invented defaults.

| Priority | Open question | Experiment / desired observation |
|---|---|---|
| **P0** | Byte layout of root block after `0x100`, exact volume-label bytes | `dinit -L` different labels on cloned images; diff block 2 only |
| **P0** | Exact `.` / `..` directory record status, signature offset and inode/reference encoding | Native images with root and nested directories; diff first 128 bytes of each directory data stream |
| **P0** | `.bitmap` inode's exact logical size for N not divisible by 8 or 4096 | QNX dinit on 4,095-, 4,096-, 4,097-block files; compare recorded length and unused bits |
| **P0** | Expected root/bitmap/`.inodes` modes, link counts, times, clean flag | Minimal `dinit -N` fixture; check every 64-byte special record and `chkfsys -f -u` |
| **P0** | Status and internal filename of used indirect inode slots | Create 17-byte name, then link/unlink; compare `.inodes` physical records |
| **P1** | Overflow extent-block header count and redundant first-extent placement | Fragment one large file into 2, 61, 62 extents using native QNX; compare all xblk records |
| **P1** | Whether link record's longname pointer is physical block or logical `.longfilenames` position | Create 49-byte and 505-byte names with native driver; inspect 0x35 field and store extents |
| **P1** | Long-name record status byte, trailing byte, alias collision behavior | Create names sharing same 48-byte prefix; delete/rename and capture before/after |
| **P1** | Whether zero loader sector is valid for native QNX4 mounts | Zero block 1 in an otherwise native-formatted data volume; mount and run read-only checker |
| **P1** | Behavior with `.inodes` indirect table growth | Tiny `dinit -i` inode table, create many >16-byte names; compare after each growth |
| **P2** | Device node major/minor encoding and `type_extension` meaning | Make block/char special files in native QNX if supported; inspect metadata bytes |
| **P2** | Busy/modified/clean flag lifecycle | Controlled native writes, snapshots after clean unmount and simulated interrupted write |
| **P2** | Legacy QNX 4.x differences (48-only, boot loader, inode initial size) | Compare QNX4.25 `dinit` against Neutrino 6.5/6.6 profiles |
| **P2** | Any nonstandard QNX4 file variants, sparse/overgrown files | Scan real industrial/embedded samples in forensic mode, preserve suspicious bytes |

### Suggested first real-world acceptance gate

1. Native `dinit -N` creates a 16 MiB reference.
2. fsimg's reader/validator extracts root special entries and matches the independently read binary layout.
3. Native QNX writes a 15-, 16-, 17-, 48-byte filename, one empty and one 513-byte regular file, a symlink, and a nested directory.
4. fsimg creates an **equivalent, not necessarily byte-identical** volume from the same manifest.
5. Linux QNX4 mount reads every file and name; QNX itself mounts and reads every file; `chkfsys -f -u` reports no structural problems.
6. Repeat after native QNX modifies fsimg's image in a disposable guest; fsimg reads it again.
7. Move to fragment/extents/505-byte names only after this basic gate.

## 17. References and source-license audit

URLs are listed for engineers to inspect *externally*. **No implementation source code or expressive vendor documentation is copied into this specification.** Format integers and identifiers remain necessary interoperability facts.

### 17.1 Primary public QNX documentation (publicly readable, vendor copyright; facts paraphrased)

- **[R1] Original QNX4.25 Disk & File Recovery:** standard loader/root/bitmap/rootdir order, signatures, directory status semantics, 60-extent blocks, system files.  
  https://www.qnx.com/developers/docs/qnx_4.25_docs/qnx4/user_guide/recovery.html
- **[R2] Original QNX4.25 Filesystem Manager architecture:** extent allocation, inode embedding, hardlink model, files/dirs and special root entries.  
  https://www.qnx.com/developers/docs/qnx_4.25_docs/qnx4/sysarch/fsys.html
- **[R3] QNX Neutrino Recovery Guide (6.4):** later `.longfilenames` and directory layout explanations.  
  https://qnx.com/developers/docs/6.4.0/neutrino/user_guide/lost_data.html
- **[R8] QNX 6.4.1 Recovery Guide:** 48-byte prefix and long-name metadata pointer; published inode/link/long-name layout illustration.  
  https://www.qnx.net/developers/docs/6.4.1/neutrino/user_guide/lost_data.html
- **[R9] QNX Working with Filesystems (6.4.1):** long-name enable mechanism, `-N` disabling, forbidden filename bytes, times, inodes.  
  https://support.qnx.com/developers/docs/6.4.1/neutrino/user_guide/fsystems.html
- **[R10] QNX 6.5 `dinit`:** canonical formatting utility, regular-file `-S`, inode-table `-i`, long-name `-N`, optional diskroot and labels.  
  https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/d/dinit.html
- **[R11] QNX `fs-qnx4.so` (6.6):** driver and filesystem tools, QNX MBR partition type codes, maximum UID/GID, optional features.  
  https://qnx.com/developers/docs/6.6.0_anm11_wf10/com.qnx.doc.neutrino.utilities/topic/f/fs-qnx4.so.html
- **[R12] QNX filesystem limits:** filenames, pathnames, file size, volume size.  
  https://qdn.qnx.com/developers/docs/6.4.0/neutrino/user_guide/limits.html
- **[R13] QNX `fs-qnx4.so` settings:** `noembed`, `overalloc`, `unbusy` and bitmap initialization behavior.  
  https://www.qnx.net/developers/docs/6.4.0/neutrino/utilities/f/fs-qnx4.so.html
- **[R14] QNX `chkfsys` manual:** nonrepairing `-f`, unconditional `-u`, raw-device `-m`, bitmap reconciliation, clean-flag behavior.  
  https://qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/c/chkfsys.html
- **[R15] QNX `mount` manual:** native `mount -t qnx4 /dev/hd0t77 /mnt/fs` example.  
  https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/m/mount.html
- **[R17] QNX original symbolic-link documentation:** symlink target is file contents, not a hard-link record.  
  https://www.qnx.com/developers/docs/qnx_4.25_docs/qnx4/sysarch/fsys.html
- **[R19] QNX case-sensitivity table:** QNX4 supports case-sensitive and case-preserving names.  
  https://qnx.com/developers/docs/6.4.0/neutrino/utilities/m/mcd.html

### 17.2 Independent open-source implementations (reference/test only; **not BSD-licensed**)

- **[R4] Linux QNX4 kernel read-only implementation**, GPL-2.0-only; entry point and driver internals:  
  https://github.com/torvalds/linux/tree/master/fs/qnx4
- **[R5] Linux public header listing 64-byte inode, link, and extent-block disk fields**, GPL-2.0 with Linux-syscall-note (still **not BSD**):  
  https://github.com/torvalds/linux/blob/master/include/uapi/linux/qnx4_fs.h
- **[R6] Linux QNX type-width definitions**, GPL-2.0 with Linux-syscall-note:  
  https://github.com/torvalds/linux/blob/master/include/uapi/linux/qnxtypes.h
- **[R7] Linux `inode.c`**: read-only mount, root detection, extent traversal, metadata loading, and conventional name max; GPL-2.0-only:  
  https://codebrowser.dev/linux/linux/fs/qnx4/inode.c.html
- **Linux `dir.c`**: fixed-slot iteration; GPL-2.0:  
  https://codebrowser.dev/linux/linux/fs/qnx4/dir.c.html
- **Linux `namei.c`**: path resolution; GPL-2.0:  
  https://codebrowser.dev/linux/linux/fs/qnx4/namei.c.html
- **Linux `bitmap.c`**: bitmap accounting; GPL-2.0:  
  https://codebrowser.dev/linux/linux/fs/qnx4/bitmap.c.html
- **Linux `qnx4.h`**: on-disk entry union interpretation; GPL-2.0:  
  https://codebrowser.dev/linux/linux/fs/qnx4/qnx4.h.html
- **[R16] `abrignoni/qnxprobe`**: read-only standard-library Python QNX4/QNX6/etc parser, CLI, synthetic QNX4 self-test, ZIP extraction; **MIT**. No native-QNX source fixture claimed.  
  https://github.com/abrignoni/qnxprobe
- **[R20] `a1ive/FsRover`**: read-only multi-filesystem GUI/CLI explorer supporting QNX4; **GPL-3.0-or-later**.  
  https://github.com/a1ive/FsRover
- **[R21] `fox-it/dissect.qnxfs`**: documented QNX4/QNX6 forensic Python reader; **AGPL-3.0**.  
  https://github.com/fox-it/dissect.qnxfs  
  https://docs.dissect.tools/en/latest/api/dissect/qnxfs/qnx4/index.html
- **QNX filesystem kernel modules packaged via DKMS**: may work on distributions with QNX4 disabled, but requires kernel compatibility and license review; listed packaging **GPL3**.  
  https://github.com/sickcodes/qnx-modules
- **`NetherlandsForensicInstitute/qnxmount`**: **Apache-2.0**, read-only QNX **EFS/ETFS/QNX6** parsers; **not a QNX4 implementation**, so useful for nearby tooling but not a QNX4 oracle.  
  https://github.com/NetherlandsForensicInstitute/qnxmount

### 17.3 Historical/experimental tools (quality and licenses unverified)

- **[R18] Anders Larsen, “The QNX4 file-system in Linux” (2004)**: maintainer background; link to Peter Waechtler's `dinit.tgz` Linux formatter and historical alternate read-write driver, not vouched for.  
  https://alarsen.net/posts/qnx4fs.html  
  **Direct archived formatter:** https://alarsen.net/files/dinit.tgz (archive download endpoint responds with gzip; archive contents and license are not audited)
- **Experimental historical Linux read/write driver link** (possibly unavailable):  
  http://qnxfs.narod.ru/
- **QNX 4.25 `dinit` older manual** (different options from Neutrino):  
  https://www.qnx.com/developers/docs/qnx_4.25_docs/qnx4/utils/d/dinit.html
- **QNX 4.25 `chkfsys` older manual**:  
  https://www.qnx.com/developers/docs/qnx_4.25_docs/qnx4/utils/c/chkfsys.html

### 17.4 Licensing decision table for fsimg

| Material | May use as external test/reference? | Copy code/text directly into fsimg or this spec? |
|---|---|---|
| Original public QNX manuals | Yes, study public interoperability facts | **No** proprietary/expressive text or source; summarize facts independently |
| QNX native binaries (licensed) | Yes, when legally obtained and run under terms | **No** redistribution or embedding |
| Linux QNX4 GPL-2.0 and GRUB/FsRover GPL-3.0 | Yes, external processes/VMs as compatibility oracles | **No**, not in BSD-only project |
| Fox-IT Dissect AGPL-3.0 | Yes, external independent reader | **No** |
| `qnxprobe` MIT, `qnxmount` Apache-2.0 | Yes, external test tools | **No** under user requirement *strictly BSD-licensed code only*, though permissive licenses might be separately acceptable if the policy changes |
| Old Linux formatter/third-party RW driver (unknown) | Potentially, only after security and rights audit | **No** |
| fsimg's original implementation from this specification | Yes | Yes, under fsimg's chosen BSD license, assuming genuinely independently authored |

## 18. Concise implementation milestones

1. **R0 — Recognition + structure dump:** root block, system inode records, `.bitmap`, basic slots; detect and reject corrupt/QNX6 images. Deliver `fsimg inspect`-style dump with all byte locations.
2. **R1 — Extraction:** inline names 1–16, indirect names 17–48, single/multi extents, nested directories, symlink data, metadata. Gate on at least Linux read-only and qnxprobe.
3. **R2 — Strict verifier:** allocation ledger, extent chain, inode references, special signatures, bitmap regeneration in memory, fuzz harness, corruption matrix.
4. **W0 — Single-extent data-only writer:** canonical root metadata and bitmap; use captured native fixture to resolve `.`/`..` and system-slot unknowns; pass native `chkfsys` nonrepair check.
5. **W1 — Full legacy writer:** multi-extent files, names up to 48, inode table, hard links, nested directories; native write/read cycle.
6. **R3/W2 — Extended filenames:** 505-byte storage, collision cases; native QNX 6.2.1+ compatibility; legacy Linux truncation explicitly tested.
7. **Optional:** recovery mode, `.bad_blks` support, volume label, bootable images supplied by caller, additional special device file types.

**Shipping bar:** fsimg should not mark QNX4 image creation “stable” merely because its own reader accepts it or because a Linux kernel can mount it. Require at least one **native QNX4/QNX Neutrino `fs-qnx4.so` mount + exhaustive forced `chkfsys -f -u`** for an image generated by fsimg, with all expected paths and bytes checked against a manifest. If native QNX access is unavailable, present creation as *experimental* and clearly state that verification limitation.

---

**Research caveat:** This is a detailed engineering specification from public documentation and externally readable implementations, not a transcript of proprietary QNX header files or source. Certain write-specific bytes cannot honestly be certified without native `dinit` reference images; these are explicitly labeled **C** rather than guessed. All external licenses should be rechecked at the actual revision used by the project.
