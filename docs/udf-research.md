# UDF research record

This record holds the evidence behind the [UDF format design](../specs/design-udf.md)
and the [driver design](../specs/design-udf-driver.md): the sources and their
licenses, the tools that produced observations, what each observation showed,
and the questions that remain open with the stage of the
[UDF plan](udf-plan.md) that closes each. The research draft
[`fsspecs/fsimg-udf-spec.md`](../fsspecs/fsimg-udf-spec.md) supplied questions
and a first layout, not rules; where an observation contradicts it, the
observation wins and the departure is listed in the plan.

## Sources

| Key | Source | License and use |
| --- | --- | --- |
| E1 | ECMA-167 3rd edition, June 1997: volume and file structure | Publicly readable standard; facts paraphrased, no text copied |
| U1 | ECMA TR/112, December 2023 republication: parts 2 to 7 are UDF 2.60, 2.50, 2.01, 2.00, 1.50 and 1.02 with errata | Same |
| N1 | NetBSD `sys/fs/udf/ecma167-udf.h` revision 1.17 | BSD-2-Clause; read to cross-check field offsets and constants, nothing copied |
| M1 | udftools 2.3 `mkudffs` and `udfinfo` on Ubuntu 24.04 | GPL-2.0; run as black-box tools only |
| G1 | `genisoimage` 1.1.11 (`-R -J -udf -iso-level 3`) | GPL; black-box tool only |
| P1 | pycdlib 1.22.0 | LGPL-2.1; black-box tool only |
| Z1 | 7-Zip 23.01 (`7z l`) | LGPL with unRAR restriction; black-box tool only |

The ECMA documents could not be fetched from the environment that produced
this record: the network proxy refused `ecma-international.org`. Every rule
the designs attribute to E1 or U1 was therefore taken from the draft, the N1
offsets and general knowledge of the standards, and is marked as such until
stage 1 of the plan confirms it against the published text (question Q1).
The M1, G1, P1 and Z1 observations below were made directly.

## Observation method

Images were created in a scratch directory from a project-independent tree
(`hello.txt`, an empty file, `Mixed Case.txt`, a name with a UTF-8 `é`, a
5000-byte file, a relative symbolic link, and `sub/deeper` nested
directories). A throwaway decoder, written for this record and not kept in
the tree, listed every block whose first 16 bytes form a descriptor tag with
a valid checksum and CRC, and decoded the fields named below. The tools were
run on regular files only.

| Image | Command |
| --- | --- |
| M1-hd102 | `mkudffs --new-file --media-type=hd --blocksize=2048 --udfrev=1.02`, 4096 blocks |
| M1-dvd102 | as above with `--media-type=dvd` |
| M1-hd201 | `--media-type=hd --blocksize=2048 --udfrev=2.01`, 4096 blocks; also `--read-only` and `--noefe` |
| M1-hd201-512 | `--blocksize=512`, 8192 blocks |
| M1-hd201-4096 | `--blocksize=4096`, 2048 blocks |
| M1-mbr | `--blocksize=512 --bootarea=mbr`, 8192 blocks |
| M1-cdr | `--media-type=cdr --udfrev=2.01`, 16384 blocks requested |
| M1-bdr | `--media-type=bdr --udfrev=2.50`, 16384 blocks requested |
| M1-cdrw | `--media-type=cdrw --udfrev=1.50`, 16384 blocks |
| G1 | `genisoimage -R -J -udf -iso-level 3` of the tree |
| P1 | pycdlib `new(interchange_level=3, udf='2.60')` with two files, a directory and two symbolic links |

`mkudffs` refused revisions above 2.01 for the `hd`, `dvd` and `dvdrw` media
types, so no M1 image has a metadata partition.

## Observations

### Recognition and anchors

* The volume recognition sequence starts at byte 32768 with descriptors
  every `max(2048, block size)` bytes: at 512, 1024 and 2048 bytes per block
  the three descriptors sit at 32768, 34816 and 36864; at 4096 they sit at
  32768, 36864 and 40960. (M1)
* Standalone images carry `BEA01`, one of `NSR02` (1.02, 1.50) or `NSR03`
  (2.01, 2.50), and `TEA01`, all with structure type 0 and version 1. G1 and
  P1 precede them with ISO 9660 `CD001` descriptors of types 1, 2 and 255 or
  1 and 255, so their extended area starts at the fourth or third slot. (M1,
  G1, P1)
* An anchor sits at block 256 in every image. Closed M1 images add one at
  `N - 1`; M1-hd102, M1-hd201 and M1-hd201-512 also at `N - 257`, M1-hd201-4096
  not. G1 writes an anchor into every block from 294 to the last, 151 blocks,
  each with its own block as tag location. The VAT images M1-cdr and M1-bdr
  have only the anchor at 256. (M1, G1)
* The first 32 KiB are zero in every image except M1-mbr, whose sector 0 is an
  MBR with one entry: status 0, type `0x07`, first LBA 0 and a sector count
  equal to the whole image. (M1)

### Volume descriptor sequences

| Fact | M1 | G1 and P1 |
| --- | --- | --- |
| Main sequence | block 96, 16 blocks (32 at `cdrw`) | block 32, 16 blocks |
| Reserve sequence | in the tail: block 3936 of 4096, `N - 17` at 4096-byte blocks, block 16224 at `cdrw`; block 240 or 224 in the VAT images | block 48, adjacent to the main one |
| Order | PVD, LVD, PD, USD, IUVD, TD | PVD, IUVD, PD, LVD, USD, TD |
| Tag serial number | 1 | 0 |
| Descriptor version | 2 for NSR02 volumes, 3 for NSR03 | 2 |
| VDS sequence numbers | 1 | 0 |
| USD extents | the unallocated volume-space gaps (six on M1-hd201) | none |
| PVD interchange levels | 2 and 3 | 2 and 2 |
| Volume set identifier | `0123456789abcdef` then `LinuxUDF` | 16 hex digits, different in the main and reserve PVD |

The reserve sequence decodes to the same logical volume, partition, file set
location and identifiers as the main one in every image, except for G1's
volume set identifier. (M1, G1, P1)

### Logical volume and partition

* LVD logical block size equals the block size in every image. The domain
  identifier is `*OSTA UDF Compliant` with the UDF revision as a 16-bit
  little-endian suffix (`0x0102`, `0x0150`, `0x0201`, `0x0250`); `--read-only`
  sets the soft write-protect flag (`0x02`) in the suffix's third byte, and G1
  and P1 set `0x03`. The implementation identifier `*Linux UDFFS` has suffix
  `04 05`, an OS class and OS identifier. (M1, G1, P1)
* Maps: one Type-1 map of 6 bytes (`01 06`, volume sequence 1, partition 0)
  on physical volumes; a Type-1 map and a 64-byte Type-2 `*UDF Virtual
  Partition` map on M1-cdr and M1-bdr; a single 64-byte `*UDF Sparable
  Partition` map, packet length 32, two sparing tables of 312 bytes at blocks
  160 and 16352, on M1-cdrw. (M1)
* The partition starts at block 257 (288 on M1-bdr, 1312 on M1-cdrw).
  M1-hd102 and M1-hd201 end it before the `N - 257` anchor; M1-hd201-4096 runs
  it to block 2024, past where that anchor would be. G1's partition of 38
  blocks includes block 294, which holds one of its padding anchors. (M1, G1)
* Access types: 4 (overwritable) for `hd`, 1 (read-only) for `dvd`,
  `--read-only` and G1 and P1, 2 (write-once) for the VAT images, 3
  (rewritable) for `cdrw`. Partition flags bit 0 is set everywhere; G1 and P1
  set flags `0x02` in the partition-contents entity identifier. (M1, G1, P1)
* The partition header names an unallocated-space bitmap at partition block
  0 in every M1 image except the VAT ones, the read-only ones included; G1, P1
  and the VAT images have an all-zero header. (M1, G1, P1)

### Integrity

* The integrity sequence extent starts at block 128 (64 for G1 and P1) and
  holds one LVID followed by a terminating descriptor, except M1-dvd102, whose
  2048-byte extent holds the LVID alone. M1 reserves 8192 bytes for `hd`. (M1,
  G1)
* Closed physical volumes record integrity type 1. The VAT images record 0,
  which `udfinfo` reports as closed because the VAT governs. (M1)
* The LVID's implementation use is 46 bytes: the implementation identifier,
  file and directory counts and the minimum read, minimum write and maximum
  write revisions, all equal to the domain revision in M1 images and `0x0102`
  in G1 and P1. Counts: M1 `0` files and `1` directory on empty volumes; G1 7
  files and 3 directories for its tree, the relative link counted as a file.
  Next unique ID: 16 on empty M1 volumes, 17 on VAT and sparable ones, 272 on
  G1, one above its largest. (M1, G1)
* Free-space table equals partition length minus the blocks the bitmap marks
  allocated; read-only G1 and P1 record 0. (M1, G1)

### Space bitmap

The space bitmap descriptor sits at partition block 0 with tag location 0,
records one bit per partition block with 1 meaning free, and spans
`ceil((24 + ceil(L / 8)) / B)` blocks: two at 512 bytes per block on a
7672-block partition. Its descriptor CRC length is 8, covering only the bit
and byte counts. (M1)

### File set and root

* M1 records the file set descriptor at partition block 1 (2 at 512 bytes per
  block, after the two-block bitmap) with a one-block extent and no
  terminating descriptor. G1 and P1 record it at partition block 0 with a
  two-block extent whose second block is a terminating descriptor. File set
  number and descriptor number 0, interchange levels 3 and 3, character set
  list 1. (M1, G1, P1)
* NSR03 M1 images record an empty system stream directory (file type 13) and
  name it in the file set descriptor; NSR02 ones do not. (M1)
* The root is an FE (NSR02, `--noefe`, G1, P1) or EFE (NSR03 M1) with ICB
  strategy 4, file type 4, unique ID 0 and link count `1 + subdirectories`
  (G1: root 2, `sub` 2, `deeper` 1). M1 stores it inline (ICB flags `0x03`),
  G1 and P1 in one block through a short AD (flags `0x230`: short ADs,
  non-relocatable, archive, contiguous). (M1, G1, P1)

### Directories and names

* A directory begins with a parent FID (characteristics `0x0a`: directory
  and parent, empty name) whose ICB names the parent; the root's names the
  root. Children follow, sorted by name in G1, in insertion order in P1. (M1,
  G1, P1)
* An FID's tag location is the partition block holding the FID's first byte:
  the ICB's block for an inline directory. Records are
  `align4(38 + L_IU + L_FI)` bytes with zero padding; `L_IU` is 0 everywhere.
  (M1, G1, P1)
* Each child FID's long AD implementation use is `flags 0` followed by the
  target's unique ID (G1 names `Mixed Case.txt` with 265, its FE's unique
  ID). Unique IDs in G1 and P1 equal each object's ICB volume block. (G1, P1)
* All observed names use compression ID 8. G1 stores the UTF-8 bytes of
  `café.txt` as eight-bit characters, so the name decodes as `cafÃ©.txt`, and
  7-Zip lists it that way. (G1, Z1)

### Files and symbolic links

* Regular files: file type 5; G1 and P1 use one short AD per non-empty file
  and no AD for an empty file, with logical blocks recorded equal to the
  allocated data blocks. (G1, P1)
* Owner and group are `0xffffffff` in G1 and P1, 0 in M1. Permissions:
  M1's root with mode 0755 is `0x3ca5`, the POSIX bits plus the owner's
  change-attribute bit; its stream directory `0x7ca5` adds the owner's delete
  bit. G1 directories are `0x14a5` and files `0x1084`, read and execute or
  read only, without change-attribute or delete bits. (M1, G1)
* Timestamps: type 1 with offset 0 everywhere; M1 records microseconds, G1
  and P1 whole seconds. (M1, G1, P1)
* Symbolic links (P1): file type 12 with the component stream as file data.
  `hello.txt` is one component of type 5, length 10, version 0 and the CS0
  name; `/sub/child.txt` begins with a component of type 2 and length 0. G1
  stored its symbolic link as an empty regular file in the UDF view. (P1, G1)

### Write-once and sparable media

* M1-cdr and M1-bdr are truncated to their recorded session: 300 and 320
  blocks although 16384 were requested. The VAT ICB, an EFE of file type 248
  with the VAT inline, is the image's last block; its header is 152 bytes,
  implementation use 0, previous VAT `0xffffffff`, revisions equal to the
  domain's; entries map virtual blocks 0 and 1 to partition blocks 0 and 1,
  the file set descriptor and the root. The partition descriptor declares
  16127 and 16096 blocks, extending past the end of the image. (M1)
* M1-bdr is UDF 2.50 by its domain suffix and uses a VAT, not a metadata
  partition. (M1)
* M1-cdrw's sparing tables carry `*UDF Sparing Table` with suffix revision
  `0x0150`, 32 relocation entries whose original packet is `0xffffffff`
  (unused) mapped to spare packets 288, 320, 352 and on, and a sequence
  number 0. The spare area is 1024 blocks at 288, before the partition. (M1)

### Revision labels in other tools

pycdlib's `udf='2.60'` produced NSR02, descriptor version 2, domain suffix
`0x0102` and LVID revisions `0x0102`: a UDF 1.02 volume. It is not a 2.60
oracle. 7-Zip chose the ISO 9660 view of the P1 bridge and the UDF view of
G1. (P1, Z1)

## Open questions

| Question | Evidence that closes it | Consumer |
| --- | --- | --- |
| Q1 Every rule marked E1 or U1 in the designs matches the published text, including whether a sparing table's spare packets may lie inside a partition, which NetBSD's header allows | Read ECMA-167 parts 1, 3 and 4 and TR/112 parts 2 to 7 | Stage 1; both designs |
| Q2 Main and reserve sequence equivalence: which fields real writers keep equal | NetBSD `newfs_udf` and `makefs`, Windows `format`, macOS `hdiutil` images | Verification rule |
| Q3 Anchor count and positions written by NetBSD, Windows and macOS, and whether any closed volume lacks the anchor at 256 | Same corpus | Reader and verification |
| Q4 Whether readers accept FIDs that straddle a block boundary, as the writer packs them | Linux, NetBSD, Windows, macOS read fsimg output | Writer packing |
| Q5 Inline regular files read by Linux, NetBSD, Windows and macOS | Native reads of fsimg output | Writer inline rule |
| Q6 A 2.01 volume without a system stream directory is accepted natively | Same | Writer layout |
| Q7 Which component type absolute symbolic links use in NetBSD, Linux and Windows output, and how each reads types 1 and 2 | Native writes and reads | Symbolic-link codec |
| Q8 LVID file and directory counts: what Linux, NetBSD and Windows count (links, streams, the root) | Native writes | Verification rule |
| Q9 Permission bits written for delete and change-attribute by Linux, NetBSD, Windows | Native writes | Writer encoding |
| Q10 Unique ID in FID implementation use on NSR02 volumes from other writers, and on each FID of a hard-linked file | Corpus and native hard links | Verification rule |
| Q11 Supplementary-plane characters as surrogate pairs in names, read natively | Native reads of fsimg output | Writer names |
| Q12 A 2.50 or 2.60 metadata-partition image from an independent writer | NetBSD `newfs_udf -K` or `makefs`, Windows 2.50 format, a Blu-ray ROM structure | Metadata stage |
| Q13 Space bitmap CRC length 8 and AED CRC length as other writers record them | Corpus | Reader acceptance, writer encoding |
| Q14 Integrity extent size natively rewritten on a read-write mount and close | Native write-after-create | Writer layout |
| Q15 Timestamp types, all-zero timestamps and invalid fields in the corpus, and what ECMA-167 says an all-zero timestamp means | Corpus and E1 | Reader acceptance |
| Q16 Whole-disk UDF with an MBR at sector 0 (M1-mbr): how the layer reader should treat it | Owner decision against the layer design | Layer MBR rule |
| Q17 Whether write-once images from other writers end with their VAT ICB, as M1's do | NetBSD, Windows and packet-writing images | Virtual stage |
