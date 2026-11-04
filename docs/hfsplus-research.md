# HFS Plus research record

This record holds the evidence behind the [HFS Plus format design](../specs/design-hfsplus.md)
and the [driver design](../specs/design-hfsplus-driver.md): the sources and
their standing, the tools that produce observations and their identities, the
captures' location and naming, and the result of each experiment the
[HFS Plus plan](hfsplus-plan.md#1-native-evidence-corpus) defines. The
research draft [`fsspecs/HFSPLUS_HFSX_FSIMG_SPEC.md`](../fsspecs/HFSPLUS_HFSX_FSIMG_SPEC.md)
supplied questions, not rules; where an observation contradicts it, the
observation wins and the departure is listed in the plan.

## Sources

| Input | Standing | Use |
| --- | --- | --- |
| The research draft | Research draft, original prose | Questions, invariants, test matrix. Every uncertainty in its section 20 is a gate, never a default |
| Apple TN1150 | Vendor documentation; facts paraphrased, no text or sample code reproduced | Layout, B-trees, catalog, Unicode rules, hard links, journal, wrapper |
| Apple `hfs_format.h`, `hfs_catalog.c`, `hfs_link.c` and xnu `decmpfs.h` | APSL 2.0; facts only, no code or declarations | Modern flags, the attribute key and records, private names, the link-stub tests and dates, the compression header and types |
| NetBSD `sys/fs/hfs/libhfs.c` | BSD-2-Clause | A second, independent derivation of the folding table, compared, not imported |
| Unicode 3.2.0 character data | Unicode license, notice in `third-party/Unicode-LICENSE.txt` | Input of the table generator |
| Apple `lzfse` at a pinned commit | BSD-3-Clause | The imported LZFSE and LZVN decoders, under the [codec design](../specs/design-codecs.md#imported-lzfse-and-lzvn-decoders) |
| macOS `hdiutil`, `newfs_hfs`, `fsck_hfs`, `diskutil`, `ditto`, `xattr` | Native tools on the development host and the CI runner | Captures and native qualification |
| `afsctool` (Homebrew) | GPL-3.0-only and BSL-1.0, external tool | Compressed files of each chosen type |
| `lzfse` command (Homebrew) | BSD-3-Clause, external tool | Test vectors for the LZFSE decoder |
| hfsprogs `mkfs.hfsplus` and `fsck.hfsplus` | APSL-derived binaries, external | Linux formatter and checker oracle |
| hfsfuse `hfsdump`, built from a pinned revision outside the tree | MIT and bundled terms, external | Independent reader |
| The Sleuth Kit `fls`, `istat`, `icat` | Mixed licenses, external | Independent forensic reader |
| Linux kernel `hfsplus` | GPL-2.0, reference only | Read-only mount comparison where a root Linux host exists; no code, declarations or identifiers enter the tree |

## Tool identities

### macOS development host

`sw_vers`:

```
ProductName:		macOS
ProductVersion:		26.6.2
BuildVersion:		25G83
```

Hardware architecture `x86_64`. Every Apple tool below is a universal binary
(`x86_64` and `arm64e`); the digests are of the whole file.

| Tool | Path | Identity (`what(1)`) | SHA-256 |
| --- | --- | --- | --- |
| `hdiutil` | `/usr/bin/hdiutil` | No SCCS identity; `hdiutil info` reports framework and driver 683.160.3 | `7b73b38f363c3fa1a338fb876d7bb7c9a6a8a9e5c1ba82fd74d8453a74c6795a` |
| `newfs_hfs` | `/sbin/newfs_hfs` (link into `hfs.fs/Contents/Resources`) | `PROJECT:hfs-715.160.9` | `e57e37ec1e7df3a66cb29ab4a2df9384f515df20254ed556bae81a433a073d50` |
| `fsck_hfs` | `/sbin/fsck_hfs` (link into `hfs.fs/Contents/Resources`) | `PROJECT:hfs-715.160.9`; it prints `Executing fsck_hfs (version hfs-715.160.9)` | `1329d4a15eb0f6effd450f4623cba863d520b948785b12e4ef4ac85f9bf4fe5e` |
| `diskutil` | `/usr/sbin/diskutil` | No SCCS identity and no version option | `624a8601b97ddfaa4b18dd2854c299e084cd45eb18fc152011c97615c3b9da1f` |
| `ditto` | `/usr/bin/ditto` | No SCCS identity and no version option | `638df9b6d1454be2405dbc3c69c28ac75405bb5b21c9e878166be92f8b3a1c4d` |
| `afsctool` | Homebrew 1.7.3 bottle | Reports `afsctool 1.7.2.` | |
| `lzfse` | Homebrew 1.0 bottle | `lzfse -encode\|-decode`, no version option | |

These tools ship with the operating system, so the build number identifies
them; the digests detect a changed tool within one build.

### macos-15 CI runner

Open. The macOS job in `.github/workflows/check.yml` has a step, "Native tool
identities", that prints `sw_vers` and the digest and `what(1)` identity of
each tool above, then runs the attach procedure below with `newfs_hfs` and
`fsck_hfs -n` on a disposable image, so a runner that cannot check an attached
image fails the step. This section records the first log's values.

### Linux and FreeBSD packages

| Platform | `hfsprogs` | `sleuthkit` | hfsfuse `hfsdump` |
| --- | --- | --- | --- |
| Ubuntu 24.04 (CI job and `build-aux/linux-check.sh` container) | Package `hfsprogs`, installed with `apt` in both | Package `sleuthkit`, installed with `apt` in both | Built by `build-aux/build-hfsfuse.sh` from the pinned upstream revision; `HFSDUMP` names the binary |
| FreeBSD 16 (`latest` package set, read from the catalogue) | Absent: no port provides `mkfs.hfsplus` or `fsck.hfsplus` | `sysutils/sleuthkit` 4.12.1 (CPL, GPLv2, IBM); provides `fls`, `istat` and `icat` | `filesystems/hfsfuse` 0.289 (MIT) installs `hfsdump`, `hfsfuse` and `hfstar`, and depends on `devel/libublio` and a FUSE library; the pinned build script makes the port unnecessary |

The FreeBSD host's local package database was out of date and `pkg update`
needs root, so the catalogue was read from
`https://pkg.FreeBSD.org/FreeBSD:16:amd64/latest/packagesite.pkg`; the
`quarterly` set has no catalogue under that path. Other HFS ports: `filesystems/hfsutils`
3.2.6 (GPLv2+, classic HFS only), `filesystems/libfshfs` (libyal, HFS and HFS
Plus reader library) and `filesystems/hfsexplorer`. The FreeBSD host has no
HFS Plus formatter, so FreeBSD runs consume captures made elsewhere.

### hfsfuse pin

`build-aux/build-hfsfuse.sh` builds `hfsdump` from
`https://github.com/0x09/hfsfuse`, revision
`eed780cbe0445893de6f4aa8d74461da00756984`, into a new directory outside the
tree. It builds the `hfsdump` target only, which carries its own libhfs,
utf8proc, LZVN and LZFSE sources and needs no FUSE library. On a volume
`newfs_hfs` created it prints the volume header (`hfsdump IMAGE`), file and
folder metadata (`hfsdump IMAGE stat PATH`), file contents (`hfsdump IMAGE read
PATH`), and the entry names of a folder (`hfsdump IMAGE read FOLDER`). It
reports `Readonly? 1`.

In the Ubuntu 24.04 container of `build-aux/linux-check.sh` the same build
produces `hfsdump`; it lists the root, a folder and a file of a volume
`newfs_hfs` created on macOS and reads the file back. In the same container
`fls -r` from sleuthkit 4.12.1 lists that volume's catalog, `fsck.hfsplus -n`
(`fsck_hfs` version `540.1-Linux`) reports it OK, and `mkfs.hfsplus` volumes
are accepted by `hfsdump` and `fsck.hfsplus -n`.

## Unprivileged attach procedure

The procedure needs no `sudo`.

1. `hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount IMAGE`
   attaches a raw image and prints an owned `/dev/diskN`. The device nodes
   `/dev/diskN` and `/dev/rdiskN` are owned by the attaching user.
2. `newfs_hfs -v NAME /dev/rdiskN` formats it and `fsck_hfs -n /dev/rdiskN`
   checks it, both as the user (`fsck_hfs` reports `** The volume NAME appears
   to be OK.` and exits 0 on a fresh volume).
3. `hdiutil detach /dev/diskN` ends the attachment.
4. To populate or modify a volume, `hdiutil attach -imagekey
   diskimage-class=CRawDiskImage -mountpoint DIR IMAGE` mounts it writable. The
   mount flags are `hfs, local, nodev, nosuid, noowners, mounted by USER`:
   `noowners` means files report the mounting user whatever the catalog holds.
   `hdiutil detach DIR` unmounts and detaches it.

A sparse image for these steps comes from `mkfile -n SIZE IMAGE`. Every
attachment belongs to the experiment that made it and is detached before the
experiment ends; `hdiutil info` lists nothing the experiment owns afterwards.

## Captures

Native captures live outside the repository in `~/hfsplus-captures/`, which is
the value of `FSIMG_HFSPLUS_NATIVE`. A capture is a directory named
`h-VARIANT-BYTES-OPTION`: `VARIANT` is `hfsplus` or `hfsx`, `BYTES` is the
volume size in bytes, and `OPTION` names the producer or its distinguishing
option, for example `h-hfsx-134217728-newfs`. Each capture holds the image,
a manifest of names, sizes, modes, times and checksums, and the commands that
created it. Images stay sparse or small: a capture records what it needs and no
more. A capture whose image adds nothing beyond its decoded fields
keeps `fields.json`, `fields-formatted.json`, `manifest.json` and `commands.txt`
without `image.img`.

Two project-owned Ruby tools produce and read captures. `tools/hfsplus/capture.rb`
formats a volume with a native tool (or imports an image another tool
made), optionally mounts it and populates it, unmounts it, runs `fsck_hfs -n`,
and writes `commands.txt`, `manifest.json`, `fields-formatted.json` and
`fields.json` beside the image; it detaches everything it attached, on failure
too. `tools/hfsplus/fields.rb IMAGE` decodes an image's boot area, volume
header and alternate header, tree header nodes, node maps, allocation bitmap,
private folders, Finder information and catalog records as text or JSON
(`--json`), treating the image as untrusted.

## Compression writers

Run on the host above with `afsctool` 1.7.2 and the system `ditto`, on a 64 MiB
HFS Plus image mounted writable. The files were repetitive text, so the data
compressed to a few kilobytes. `afsctool -v FILE` reported the type and the
header's uncompressed size; `ls -lO` showed the `compressed` flag; the file
contents read back identical to the original (SHA-256 unchanged for all three).

`afsctool` accepts HFS Plus volumes and writes each type on demand with
`-T`:

| Command | Result on a 300000-byte file |
| --- | --- |
| `afsctool -c -T ZLIB -v FILE` | `ZLIB in resource fork (4)` |
| `afsctool -c -T LZVN -v FILE` | `LZVN in resource fork (8)` |
| `afsctool -c -T LZFSE -v FILE` | `LZFSE in resource fork (12)` |

A 100-byte file compressed with `-s 0` is stored inline in the `decmpfs`
attribute: `ZLIB in decmpfs xattr (3)`, `LZVN in decmpfs xattr (7)` and `LZFSE
in decmpfs xattr (11)`. `ls -lO` shows `compressed` for all of them, and `xattr
-l` lists no attribute although the inline types keep their data in one, so the
kernel hides `com.apple.decmpfs`. Whether it also hides the resource fork of
types 4, 8 and 12 is for experiment E6, which reads the raw attribute and
resource fork from the volume itself.

`ditto --hfsCompression SRC DST` compresses on copy. On this macOS it wrote
LZVN, never ZLIB or LZFSE, for every compressible file it compressed:

| Source size | Result |
| --- | --- |
| 500 and 4000 bytes | Not compressed |
| 20000 bytes | `LZVN in decmpfs xattr (7)` |
| 70000, 300000 and 1000000 bytes | `LZVN in resource fork (8)` |
| 100 bytes | Not compressed |

So `afsctool` supplies ZLIB and LZFSE files, and either tool supplies LZVN.
`afsctool` describes itself as for APFS and ZFS; it carries no refusal for HFS
Plus. The `lzfse` command (Homebrew formula `lzfse` 1.0) encodes and decodes raw
LZFSE streams (`lzfse -encode -i IN -o OUT`) and supplies the stage 5 test
vectors.

## Experiments

Each experiment varies one property on disposable volumes, as the plan's stage
1 table defines. Its heading carries its status; stage 1 fills in the
procedure actually run, the observations and the table or field each
consumer needs.

### E1 Empty volumes

Status: answered for `newfs_hfs`, `hdiutil create`, `diskutil eraseVolume` and
`mkfs.hfsplus`. The wrapper options of `mkfs.hfsplus` belong to E11.

Procedure. `tools/hfsplus/capture.rb` formats each image and records the boot
area after every formatting step; `tools/hfsplus/fields.rb` decodes the result
into the capture's `fields-formatted.json`, and `fsck_hfs -n` checks it on the
attached device. 50 `newfs_hfs` volumes (`h-*-newfs-*`): 1, 32, 128 and 1024
MiB at block sizes 512 to 65536 (`-b`), and 32 and 1024 MiB at 512, 4096 and
65536 with `-s`, `-J` and `-s -J`. 6 `hdiutil create -layout NONE` volumes
(`h-*-hdiutil-*`), converted to a raw image: `HFS+`, `HFS+J`, `HFSX` and
`JHFS+X` at 32 MiB, `HFS+` at 1 and 256 MiB. 4 `newfs_hfs` volumes over an
image that held a FAT16 or FAT32 volume (`h-*-overfat*`), 4 `diskutil
eraseVolume` volumes over FAT16 (`h-*-diskutil-overfat16-*`), and 18
`mkfs.hfsplus` volumes from the Ubuntu 24.04 container of
`build-aux/linux-check.sh` (`h-*-mkfs-*`, imported). `mkfs.hfsplus` and `fsck.hfsplus` report themselves as 540.1-Linux.
`fsck_hfs -n` reports every non-journaled volume OK; it reports a journaled
volume only as journaled and not checked, so journaled volumes are checked by
the decoder's own consistency fields (below).

Volume header, every formatter and option (87 volumes decoded before any
mount of ours; the E2 volumes are counted before their population). The tables below
hold for all of them unless a row names an exception.

| Field | Observation |
| --- | --- |
| Signature, version | `H+`/4, or `HX`/5 with `-s`, `HFSX` and `JHFS+X` |
| `attributes` | `0x80000100` (clean and unused nodes zeroed, bits 8 and 31); `0x80002100` with the journaled bit 13 added. Bit 31 is set by every formatter, so the design's `0x00000100` is not what native volumes carry |
| `lastMountedVersion` | `10.0` on `newfs_hfs`, `mkfs.hfsplus` and unjournaled `hdiutil` volumes; `HFSJ` on volumes `hdiutil` and `diskutil` journaled by mounting them |
| `writeCount` | 0; 1 on `hdiutil` journaled volumes (`h-hfsplus-33554432-hdiutil-hfsplusj`) |
| `encodingsBitmap` | 1 (MacRoman only), also after the E2 population |
| `fileCount`, `folderCount` | 0 and 0; with `-J` the file count is 2 (the journal files, IDs 16 and 17) and the folder count 0 |
| `nextCatalogID` | 16; 18 with `-J`; 20 on an `hdiutil` journaled volume, whose mounting also created the two private folders (IDs 18 and 19, counted in `folderCount`, which is 2) |
| Finder information | words 0 to 5 zero everywhere; words 6 and 7 hold a volume identifier that differs for every volume |
| Dates | `modifyDate` equals `checkedDate`, `backupDate` is 0. `createDate` of `newfs_hfs`, `hdiutil` and `diskutil` is local wall-clock time, offset from `modifyDate` by the host's UTC offset; `mkfs.hfsplus` writes the same UTC value in `createDate`, `modifyDate` and `checkedDate` |
| Clump sizes | `dataClumpSize` = `rsrcClumpSize` = `min(16 * B, 65536)`; the extents and catalog forks' `clumpSize` equals their `logicalSize`; see the tree table |
| `nextAllocation` | A hint inside the free area (114 at 1 MiB, 1474 at 32 MiB, 5890 at 128 MiB, 23561 at 1 GiB, all with `B` 4096); not derived from the end of any file |
| Boot area | Bytes 0 to 1023 are all zero on every volume; no `LK` boot block appears. SHA-256 of the 1024 zero bytes is in each manifest |
| Alternate header | Equal to the primary on 81 of 87 volumes. The six exceptions were mounted by their formatter, the two `hdiutil` journaled volumes and the four `diskutil eraseVolume` volumes: the alternate lacks the mount's effects (`folderCount`, `nextCatalogID`, `writeCount`, and on the `diskutil` volumes also `fileCount`, `freeBlocks`, `nextAllocation`, `modifyDate`, and `lastMountedVersion` when journaled) |
| Private folders | None on `newfs_hfs`, `mkfs.hfsplus` and unjournaled `hdiutil` volumes. The `hdiutil` and `diskutil` journaled volumes hold both, created by mounting (E2 gives their records) |

Object order and trees. Every empty volume lays out, in block order, the
allocation file, the extents tree, the attributes tree and the catalog tree
(`h-hfsplus-33554432-newfs-b4096`: bitmap at block 1, extents 2+64, attributes
66+64, catalog 770+64; the free blocks between the attributes and the catalog
tree are zero). With `B` 512 the bitmap starts at block 3 (boot area and
header occupy blocks 0 to 2). Block 0 and the last block are allocated.
`-J` places the journal info block and the journal first: with `B` 4096 at 32
MiB the journal info block is block 2, the journal begins at block 3 and the
trees follow at 131, 195 and 899. Node sizes and counts, volume headers'
`totalBlocks * B` equal to the image size on all volumes:

| Volume | Extents tree | Catalog tree | Attributes tree | `clumpSize` of the three forks |
| --- | --- | --- | --- | --- |
| 1 MiB, any `B` but 65536 | 8 nodes of 4096 | 8 of 4096 | 8 of 8192 | 32768, 32768, 65536 |
| 1 MiB, `B` 65536 | 16 of 4096 | 16 of 4096 | 8 of 8192 | 65536 each |
| 32 MiB, any `B` | 64 of 4096 | 64 of 4096 | 32 of 8192 | 262144 each |
| 128 MiB, any `B` | 256 of 4096 | 256 of 4096 | 128 of 8192 | 1048576 each |
| 1 GiB, `B` 512 to 2048 | 1024 of 4096 | 1024 of 4096 | 512 of 8192 | 4194304 each |
| 1 GiB, `B` 4096 to 65536 | 1024 of 4096 | 512 of 8192 | 512 of 8192 | 4194304 each |

Every tree has its header node and nothing else except the catalog, which has
the root folder's record and thread in one leaf: depth 1, root node 1,
`leafRecords` 2 (6 with `-J`), first and last leaf 1. Extents and attributes
trees have depth 0, root 0 and no leaf. `maxKeyLength` is 10, 516 and 266;
`btreeType` is 0 in all three; `keyCompareType` is 0, `0xCF` (`H+`) or `0xBC`
(`HX`), and 0; the flags are `0x02` for extents and `0x06` for catalog and
attributes. `reserved1`, the 64 `reserved3` bytes and the 128 user-data bytes
of every header node are zero. The header map record is `nodeSize - 256`
bytes, no map nodes exist, the map's set bits equal `totalNodes - freeNodes`,
and every unused node is all zero on every volume. The bitmap's clear bits
equal `freeBlocks`, and the bits past `totalBlocks` are clear. The decoder
also reads the free blocks: on the 31 volumes captured with that check (every
FAT-over, `mkfs.hfsplus`, `diskutil` and E2 volume) every free block is zero.
The fork of each tree is one extent.

Root folder record. `newfs_hfs` and `hdiutil`: `createDate` = `contentModDate`
= the format time in UTC, `attributeModDate`, `accessDate` and `backupDate` 0,
owner = the formatting user, group 20, mode `040755`, `textEncoding` 126,
flags `0x0000`, valence 0, Finder information zero, no thread flag. With `-s`,
`HFSX` and `JHFS+X` the flags are `0x0010` (folder count valid) and the folder
count is 0. `mkfs.hfsplus`: owner 0, group 0, mode 0 (no file type bits),
`textEncoding` 0, flags `0x0000` also for HFSX, and the same dates in UTC.

Variants.

* `-s` / `HFSX`: signature `HX`, version 5, `keyCompareType` `0xBC`, root
  flags `0x0010`; everything else equals the case-insensitive volume of the
  same geometry (`h-hfsx-33554432-newfs-s-b4096`).
* `-J` / `HFS+J` / `JHFS+X`: journaled bit set, `journalInfoBlock` the first
  block after the bitmap and the header area (2 at 32 MiB with `B` 4096 or
  65536 and at 1 GiB with `B` 65536; 19 at 32 MiB with `B` 512; 9 at 1 GiB
  with `B` 4096; 515 at 1 GiB with `B` 512), a journal
  of 512 KiB at 32 MiB and 8 MiB at 1 GiB, the journal files `.journal` and
  `.journal_info_block` in the root (type `jrnl`, creator `hfs+`, flags
  `0x0002`, owner 0, mode `0100000`, journal data fork 524288 bytes, journal
  info block one block). `newfs_hfs -J` and `mkfs.hfsplus -J` leave the
  journal info block flags at `0x00000005` and the journal header zero (not
  yet initialized); `hdiutil` initializes it: flags `0x00000001`, journal
  header magic `0x4A4E4C78` and endian word `0x12345678` stored little-endian
  (the creating host's order), `start` = `end` = 17408, `blhdr_size` 8192,
  `jhdr_size` 512. E8 owns the remaining journal rows.
* `hdiutil` `HFS+` and `HFSX` equal `newfs_hfs` with `B` 4096, whatever the
  size (1, 32 and 256 MiB).
* `mkfs.hfsplus` produces the same trees, clump sizes and layout as
  `newfs_hfs` at every size and block size (`h-hfsplus-33554432-mkfs-b4096`
  against `h-hfsplus-33554432-newfs-b4096`); only the root folder record and
  the dates differ, as above. `-b` below 4096 prints a warning that the block
  size is smaller than the catalog node size and still succeeds. `-n
  c=8192,e=8192` gives 32 nodes of 8192 for the extents and the catalog tree.
  `-J 8M` still yields a 512 KiB journal at 32 MiB. `-b 65536` (1 and 32
  MiB) crashes the formatter with a segmentation fault, and `-n a=16384`
  aborts with a detected buffer overflow; neither produces an image.

Formatters over a FAT volume. `newfs_hfs` (HFS+, `-s`, `-J`, and over FAT32),
`diskutil eraseVolume` (`HFS+`, `JHFS+`, `HFSX`, `JHFSX`) and `mkfs.hfsplus`
leave bytes 0 to 1023 zero over an image whose first sector held a FAT boot
sector (`eb3c9042...` for FAT16, `eb589042...` for FAT32, recorded in the
manifests after the FAT step), and every free block of the new volume is
zero: no stale FAT sector survives in the free area. For `mkfs.hfsplus` the
FAT volume came from `mkfs.vfat -F 16`, which reported success; its boot
sector was not read back before formatting (the container has no `xxd`). `hdiutil create` makes
a new image, so it has no in-place case; `diskutil eraseVolume` is the
unprivileged in-place route and calls the same formatter.

Evidence rows answered:

* Boot area bytes of every formatter's output: all zero, none begins with
  `LK`, for `newfs_hfs`, `hdiutil`, `diskutil` and `mkfs.hfsplus`. The probe's
  second part (zero or `LK`) holds for every formatter observed.
* Volume attribute bits, catalog flags, B-tree flags, `keyCompareType`,
  `maxKeyLength` and node sizes on native volumes, before use: the tables
  above; E2 gives the state after use. The B-tree flag bit 0 (bad close) is
  clear everywhere. Open until E10: old volumes.
* Header fields the alternate keeps equal after native use: E2.
* `newfs_hfs` empty-volume layout: above. The root owner, mode and dates and
  the `createDate` semantics are the root record and date rows above;
  `writeCount` and `encodingsBitmap` are 0 and 1.
* Whether `newfs_hfs` creates the private folders and an attributes tree, and
  sets folder counts: it creates an attributes tree on every volume and no
  private folders; folder counts are valid (flag `0x0010`) on the root of HFSX
  and not on HFS Plus.
* Whether a formatter clears another volume's boot sector: yes, all three
  in-place formatters.

### E2 Population

Status: answered, with one open item (owners other than the mounting user).

Procedure. `capture.rb --populate e2` formats a volume, mounts it writable
with `hdiutil attach -mountpoint`, creates the tree of its manifest, unmounts,
and decodes the image. Five volumes: `B` 4096 (`h-hfsplus-33554432-e2-b4096`),
512 (`h-hfsplus-33554432-e2-b512`), 65536 (`h-hfsplus-67108864-e2-b65536`),
HFSX (`h-hfsx-33554432-e2-s-b4096`) and journaled
(`h-hfsplus-33554432-e2-J-b4096`). The tree holds files of 0, 1, `B - 1`,
`B`, `B + 1` bytes; a chain of 8 nested folders with a file in the last; files
named by 1, 254 and 255 units (a 256-unit name is attempted); names with `:`
leading, inside, trailing and doubled; symbolic links of 1, 255, 1023, 1024,
1025 and 2048 bytes; a FIFO; files at times 1904-01-01, 1969, 2040-02-06
06:28:15 (the maximum) and 2222222222 (beyond it); a folder at 1904 and a
symbolic link at 1969; files of modes `0600`, `0755`, `4755`, `0000`; folders
of modes `1777` and `0500`; and a `chown` to 0 and a `chgrp` to another group
of the user. Every refusal is recorded in `manifest.json` under `attempts`.
`fsck_hfs -n` reports every non-journaled populated volume OK, and the decoder
finds no problem on any of them.

Refusals. A 256-unit name fails with `ENAMETOOLONG`; symbolic links of 1024,
1025 and 2048 bytes fail with `ENAMETOOLONG`; `chown` to uid 0 fails with
`EPERM`. Everything else, including the 255-unit name and the 1023-byte link,
is created. The 1023-byte link's data fork is 1023 bytes; the longest
target macOS stores is 1023 bytes, and a longer one cannot be created on
macOS. Whether macOS reads a longer target written by another tool stays open
(it blocks only a decision to accept 1024 or more, which the design refuses).

Catalog records (all five volumes agree unless stated).

| Object | Observation |
| --- | --- |
| Files of `B - 1`, `B` bytes | one extent of one block; `B + 1` two blocks; 0 bytes no extent and 0 blocks. Same at `B` 512, 4096 and 65536. Allocation is contiguous in creation order from the volume's allocation hint |
| File record | flags `0x0082` (thread, date added); `clumpSize` 0 in both forks; resource fork empty; BSD `special` 1 (also on folders, links and FIFOs); user information zero; the last 8 bytes of the Finder information are `dateAdded` (a Unix time, bytes 4 to 7) and a write counter (bytes 12 to 15: 1 for an empty file, 2 after a write, 0x21 on the root) |
| Folder record | flags `0x0080`; Finder information as for a file, counter 0 to 4; on HFSX flags `0x0090` and a folder count in the reserved word (`n1` holds 1) |
| Symbolic link | file record, mode `0120755`, flags `0x0002` (no date-added flag), Finder information zero, user information type `slnk` creator `rhap`, `textEncoding` 0, data fork = target, 1 block (2 for 1023 bytes at `B` 512) |
| FIFO | file record, mode `010644`, flags `0x0002`, forks empty, user information zero |
| Names | 255 units stored whole. `:` is stored as `/`: `colon:mid` is `colon/mid`, `:lead` is `/lead`, `trail:` is `trail/`, `a::b` is `a//b`; a `/` cannot be created from the POSIX side |
| Permissions | modes `0000`, `0600`, `0755`, `4755`, `1777` and `0500` are stored exactly, with the type bits (`0100000`, `040000`, ...); owner is the mounting user's uid and group 20 |
| Times | see below |

Times, as stored (create, contentMod, attributeMod, access):

| Set to | Stored |
| --- | --- |
| 1904-01-01 (file, folder) | `createDate` 0 and `contentModDate` 0 (the creation date is lowered to the modification date); attributeMod and access are the write time |
| 1969 (file) | create and contentMod both 2065844800; attributeMod and access are the write time |
| 1969 (symbolic link, `lutime`) | create, contentMod and access all 2065844800 |
| 2040-02-06 06:28:15 | contentMod 4294967295; create is the write time |
| 2222222222 Unix | contentMod 10099726: the value is reduced modulo 2^32 and reads back as a 1904 date. While mounted, `lstat` reports the unreduced 2222222222 |

A regular file's access date keeps the write time whatever `utimes` sets; the
symbolic link's follows. `backupDate` is 0 everywhere.

What macOS adds. The first mount creates the two private folders of the hard-link design: `\0\0\0\0HFS+ Private Data` (ID 16) and
`.HFS+ Private Directory Data\r` (ID 17), both children of the root with flags `0x0010`, valence 0, owner 0,
group 0, mode `040000` and `041555`, owner flags `2`, and user information
`00000000 00000000 5000 4000 4000 0000`. On unmount, a folder `.fseventsd`
(flags `0x008C` on HFS Plus, `0x009C` on HFSX; mode `0700`, owner the mounting
user) with a file `fseventsd-uuid` is added to the root, so the root's valence is the user's
entries plus 3 (29 plus 3 = 32 on the 4096-byte volume) and the `fileCount` counts the
files inside it. The catalog's
attributes tree gains one record, `com.apple.system.Security`, on the
`.fseventsd` folder (inline data of 68 bytes), and the folder carries flags
`0x0004` and `0x0008`. No Spotlight or Trash folder is created. On HFS Plus
the root keeps flags `0x0000` and a folder count of 0; on HFSX the root keeps
flag `0x0010` and counts its child folders in the reserved word (7 here).
The catalog of the populated 4096-byte volume has depth 2 (88 leaf records in
5 leaves under one index node); the extents tree stays empty.

Volume bits and the alternate header.

| State | Observation |
| --- | --- |
| Formatted | `attributes` `0x80000100`, `writeCount` 0 |
| Mounted (header read during the mount, after `sync`, on a throwaway volume) | `attributes` `0x80000000`: the clean bit is clear, bit 31 stays set; `writeCount` 1; `modifyDate` advanced; `folderCount` and `nextCatalogID` already include the two private folders created by the mount; `fileCount` still 0 |
| Unmounted after the E2 population | `attributes` `0x80000100` again (`0x80002100` journaled); `writeCount` 22 to 26 (one per write session of the mount); `lastMountedVersion` `10.0` (`HFSJ` journaled); `encodingsBitmap` 1; `fileCount`, `folderCount` and `nextCatalogID` equal the catalog (29 files, 14 folders besides the root, next ID 59 on the 4096-byte volume, whose `.fseventsd` holds three entries; 27 files and next ID 57 on the 512-byte one) |
| Alternate after the same session | Signature, version, `blockSize`, `totalBlocks`, the five fork-data records, `createDate`, `checkedDate`, `backupDate`, Finder information, `encodingsBitmap`, the clump sizes and `journalInfoBlock` equal the primary. `fileCount`, `folderCount`, `freeBlocks`, `nextAllocation`, `nextCatalogID`, `writeCount` and `modifyDate` differ (`lastMountedVersion` too when journaled): the alternate keeps the formatted state |
| Alternate after the catalog tree grew during a mount (a throwaway volume with 3000 files, catalog from 64 to 640 nodes, depth 4) | The alternate's catalog fork equals the primary's (one extent of 640 blocks); its `attributes` is `0x80000000` and its counts, `writeCount` and `modifyDate` are those of the moment the tree grew |

So an alternate header is rewritten when a special file's extent changes, and
at no other time; its `attributes` can lack the clean bit. The five fork-data
records and the geometry are the only fields guaranteed equal.

Owners. A volume mounted by `hdiutil attach` has `noowners`: `lstat` reports
the mounting user and group 20 whatever the catalog holds, and `chgrp`
succeeds without being stored. Stored values were therefore read from the
image: created entries store the mounting user and group 20 (the private
folders store owner 0). `diskutil enableOwnership` fails with "You must be
root". `hdiutil attach -owners on` mounts without `noowners` and needs no
privilege: `chgrp` to another group of the user is then stored (group 80 read
back from the catalog, owner unchanged, mode `0104755` after `chmod 4755`),
and `chown` to another uid fails with `EPERM`. Stored owners other than the
mounting user and root (the private folders and the `mkfs.hfsplus` volumes
supply uid 0) cannot be produced unprivileged on macOS. That is open; it
blocks only a test of the reader against a native volume with a third owner.

Evidence rows answered:

* Header fields the alternate header keeps equal after native use: the table
  above. Verification may require equality of the signature, version, block
  size, total blocks and the five forks, and should not compare `attributes`.
* Volume attribute bits and the other flags on native volumes after use: the
  clean bit clears while mounted and returns on unmount; bit 31 is always set;
  no bit outside the design's table appears. Catalog flags seen: `0x0002`,
  `0x0004`, `0x0008`, `0x0010`, `0x0080`; no `0x0020`, `0x0040` or `0x1000`.
  The B-tree flags stay as formatted, with the bad-close bit clear. Open until
  E10 for older volumes.
* Longest symbolic-link target macOS stores and the Finder codes it writes:
  1023 bytes; `slnk`/`rhap`.
* Whether `newfs_hfs` sets folder counts: E1 and the folder row above (HFSX
  keeps them current; HFS Plus does not set the flag on user folders).
* Rows left open here because the experiment that feeds them is another:
  the map-node record length (E9), the Unicode rows (E3), the hard-link,
  attribute, compression and journal rows (E4 to E8).

### E3 Unicode

Status: open.

### E4 Hard links

Status: open.

### E5 Attributes

Status: open.

### E6 Compression

Status: open. The writers above are established; the header, the raw markers,
the type 4, 8 and 12 framings, the largest inline size and the data fork's size
remain.

### E7 Fragmentation

Status: open.

### E8 Journal

Status: open.

### E9 Large trees

Status: open.

### E10 Old volumes

Status: open.

### E11 Wrapper

Status: open.
