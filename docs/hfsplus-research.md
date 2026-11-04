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
(`--json`), treating the image as untrusted; it also decodes the attributes and
extents overflow leaf records and, for compressed files, the `decmpfs` header and
the resource fork's chunk table; `--index` walks the index nodes of each tree and
compares every index key with the first key of the child it names. `capture.rb
--populate` takes the profiles `e2`, `e4`, `e5`, `e6-zlib`, `e6-lzvn`,
`e6-lzfse`, `e6-ditto`, `e6-sweep`, `e7`, `e9-create`, `e9-delete`, `e9-thin` and
`e9-sparse` (`tools/hfsplus/populate*.rb`); `tools/hfsplus/patch_record.rb`
rewrites fields of one catalog file record in a copy of an image, and
`tools/hfsplus/e8_interrupt.rb` interrupts mounted journaled sessions and decodes
the journal that is left.

### Throwaway probes

Four observations in E1 and E2 come from probes typed by hand, with no capture
directory or `commands.txt` behind them. Each can be repeated with the commands
below; `V` is a 32 MiB image made with `mkfile -n 33554432 V`, formatted with
`newfs_hfs -v FSIMG -b 4096` on the raw device of `hdiutil attach -imagekey
diskimage-class=CRawDiskImage -nomount V`, and detached again.

* Mounted volume header (`attributes` `0x80000000`, `writeCount` 1). Mount `V`
  with `hdiutil attach -imagekey diskimage-class=CRawDiskImage -mountpoint MNT
  V`, run `sync`, and decode the image file while it is mounted with
  `ruby tools/hfsplus/fields.rb V`.
* Alternate header after the catalog tree grew (3000 files, catalog from 64 to
  256 nodes, depth 3). Mount `V` as above, create the files with `for i in $(seq
  1 3000); do : > MNT/f$i; done`, run `sync`, detach, and decode with
  `ruby tools/hfsplus/fields.rb V`. The node counts depend on the volume size,
  block size and file names; the alternate header differs from the primary in
  `attributes`, the counts, `writeCount` and `modifyDate`.
* `mkfs.hfsplus -b 65536` segmentation fault and `-n a=16384` abort. In the
  Ubuntu 24.04 container of `build-aux/linux-check.sh` with hfsprogs installed,
  run `truncate -s 32M V`, then `mkfs.hfsplus -b 65536 V` and `mkfs.hfsplus -n
  a=16384 V`, and read the exit status of each.
* `hdiutil attach -owners on` and `chgrp`. Mount `V` with `hdiutil attach
  -imagekey diskimage-class=CRawDiskImage -owners on -mountpoint MNT V`, then
  `: > MNT/g; chgrp 80 MNT/g; ls -ln MNT/g`, detach, and read the record of
  `g` from `ruby tools/hfsplus/fields.rb --json --records 5000 V`: its `group`
  is 80 and its `owner` the mounting user.

Probes of E8, E9 and E11, each a hand-typed observation outside a capture
directory. `V` is a 32 MiB image made with `mkfile -n 33554432 V` and formatted
with `newfs_hfs -J -v LV -b 4096` through `hdiutil attach -imagekey
diskimage-class=CRawDiskImage -nomount V`, detached again.

* Journal while mounted (E8). Mount `V` with `hdiutil attach -imagekey
  diskimage-class=CRawDiskImage -mountpoint MNT V`, run `sync`, and decode the
  image while it is mounted with `ruby tools/hfsplus/fields.rb V`: three of three
  runs report `start` 512, `end` 17408 and `attributes` `0x80002000`, and
  `first_transaction` holds `num_blocks` 4 and `bytes_used` 16896. After `hdiutil
  detach MNT` the same decode reports `start` equal to `end` and `attributes`
  `0x80002100`.
* Journaling disabled (E8). Mount `V` as above, write one file, run `diskutil
  disableJournal MNT` (it needs no privilege and prints `Journaling has been
  disabled for volume`), detach, and decode: capture
  `h-hfsplus-33554432-e8-journal-disabled` holds the result.
* File creation speed (E9). Format a 64 MiB `V` with `newfs_hfs -v S`, mount it,
  and time `Dir.mkdir("MNT/t"); 3000.times { File.write(format("MNT/t/file-%05d",
  _1), "") }` in Ruby: 0.52 seconds.
* Native `nextAllocation` (E9). `mkfile -n 671088640 V`, attach, `newfs_hfs -v E9
  -b 4096 -n c=4096 -c c=76800` on the raw device, detach. `fsck_hfs -n` on the
  attached device reports `Volume header needs minor repair` and exits 8. Writing
  92166 (the block after the catalog) over the four bytes at 1024+52 of `V`, which
  hold 860166, makes `fsck_hfs -n` report the volume OK.
* Index keys after deletions (E9). Read the root of the catalog tree of
  `h-hfsplus-671088640-e9-create` and of `h-hfsplus-671088640-e9-delete`, collect
  the key of every record of every child of the root
  and subtract the sets. After `gunzip -k` of the two `image.img.gz`, `ruby
  tools/hfsplus/index_keys.rb CREATE.img DELETE.img` prints 8203 keys before, 7960
  after, 2343 after that are not before.
* Wrapped journaled volume mounted (E11). Copy
  `h-hfsplus-16777216-e11-mkfs-w-J/image.img` to `V`, mount it with `hdiutil
  attach -imagekey diskimage-class=CRawDiskImage -mountpoint MNT V` (macOS mounts
  it: `hfs, local, nodev, nosuid, journaled, noowners, mounted by USER`), create a
  file, detach, and decode. The journal info block's `offset` changes from 12288
  to 57344 and the journal header is found at image byte 57344.

Further probes of E4 to E6, each a hand-typed observation outside a capture
directory.

* Look-alike stubs (E4). Copy `h-hfsplus-8388608-e4/image.img` to `V`, read the
   `createDate` of the root folder (ID 2) and of the private data folder (ID
   16) from its `fields.json`, and patch ten of the `stubtest-*` files:
   `ruby tools/hfsplus/patch_record.rb V stubtest-a --type hlnk --creator hfs+
   --create-date PRIVATE --special 21 --flags 0x22` for `a`, with `--create-date
   ROOT` for `b`, no `--create-date` for `c`, `--creator 0x00000000` for `d`,
   `--type 0x00000000` for `e`, no `--flags` for `f`, and `--type fdrp --creator
   MACS --special 20 --flags 0x22` for `g` (`h` without `--create-date`, `i`
   without `--flags`, `j` with `--special 5`). Mount `V` writable and run
   `stat -f 'ino=%i nlink=%l size=%z' stubtest-X; cat stubtest-X` on each.
   `ROOT` is the date after the profile's `SetFile -d` step; the run shows the
   private date and the root date both read as links.
* Link codes (E4). On a mounted fresh volume run `xattr -wx
   com.apple.FinderInfo
   686c6e6b6866732b000000000000000000000000000000000000000000000000 f`
   (`Operation not permitted`), `SetFile -t hlnk -c hfs+ f` and `SetFile -t hlnk
   f` (both `ERROR: Unexpected Error. (-5000)`, exit 2), `SetFile -t fdrp -c
   MACS f` (exit 0), `ln -s f s; ln -P s s2` (`Operation not supported`) and
   `mkdir d; ln d d2` (`ln: d: Is a directory`).
* Attribute names (E5). On a mounted fresh volume `xattr -w NAME v f` with NAME
   of 127 `b` characters succeeds and with 128 fails with `[Errno 63] File name
   too long`.

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
| Alternate after the catalog tree grew during a mount (a throwaway volume of 32 MiB and 4096-byte blocks with 3000 empty files, catalog from 64 to 256 nodes, depth 3) | The alternate's catalog fork equals the primary's; its `attributes` is `0x80000000` and its counts, `writeCount` and `modifyDate` are those of the moment the tree grew |

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

Status: answered, with two open items (hard-linked symbolic links; the
directory hard-link stub test).

Procedure. `capture.rb --populate e4` on `h-hfsplus-8388608-e4` and
`h-hfsx-8388608-e4` (block size 4096; `tools/hfsplus/populate_links.rb`). The
tree holds one file with two names in two folders, one with three names in
three, groups of three and two names unlinked until one, or none, is left, a
link to a symbolic link, a directory link, mode and time changes made through
one name, and ordinary files given link and alias Finder codes. `fsck_hfs -n`
reports both volumes OK and the decoder finds no problem. HFS Plus and HFSX
agree on every field below. The last step sets the root folder's creation date
earlier with `SetFile -d` and makes one more link (`rd/`).

What macOS adds. The first writable mount creates the private data folder
(ID 16, flags `0x0010`, valence 0, owner 0, `createDate` set and the other
dates 0) and `.HFS+ Private Directory Data\r` (ID 17). The first `link(2)` of a
file `F` turns `F` into the indirect node and creates two stubs: the node
`iNodeF` (named with the decimal CNID of `F`, kept) sits in the private data
folder, and every name, the first included, is a new stub record with a new
CNID.

| Record | Observation |
| --- | --- |
| Stub | `flags` `0x0022` (thread, hard-link chain); file type `hlnk`, creator `hfs+`, Finder flags `0x0100`; both forks empty; text encoding 126; BSD `mode` `0100444`, `ownerFlags` `0x02`, `special` the indirect node's CNID; all four dates equal the private data folder's `createDate` (not the file's, not the root's); the 4-byte word at record offset 4 is 0 |
| Indirect node | `iNodeN` in folder 16; `flags` `0x00A2` (thread, chain, date added); no Finder type or creator; BSD owner, group, mode and times of the file; `special` the link count; the word at record offset 4 holds the CNID of the newest stub; forks hold the content |
| Chain | the node's word at offset 4 names the newest stub, the head; a stub's BSD `ownerID` is the next stub toward the head (0 at the head) and its `groupID` the next toward the oldest (0 at the tail). Three names, ids 29, 30, 31: node word 31; stub 31 owner 0 group 30; stub 30 owner 31 group 29; stub 29 owner 30 group 0 |
| Unlinked to one | the node stays, `special` 1, its word names the remaining stub, and that stub stays a stub (`0x0022`, owner 0 group 0, `special` the node). A file whose last name is unlinked loses node and stubs |
| Attributes on nodes | none. The attributes tree holds `com.apple.system.Security` (ACLs and `.fseventsd`) and no `com.apple.system.hfs.firstlink`; macOS keeps the head in the node's record word |

Mounted view. `stat` of every name reports the indirect node's inode number
(its CNID), mode, owner and times, and `nlink` from the node's `special`. After
`chmod 0600` and a time change through the second name (`meta/b`), `meta/a`
reports mode `0600` and the new time, and the node holds both: its `createDate`
and `contentModDate` moved with the time. The stub's `0444` and `ownerFlags`
are never reported. The same field gives an ordinary file its `nlink`: files
patched to `special` 20 and 5 report `nlink` 20 and 5.

Stub test. macOS refuses to create a file with the `hlnk`/`hfs+` codes:
`xattr -wx com.apple.FinderInfo` fails with `EPERM`, and `SetFile -t hlnk`
(with or without `-c hfs+`) exits 2 with error -5000 (the link-codes probe), so every
finding below comes from `patch_record.rb`, which rewrites one record of a
copy of `h-hfsplus-8388608-e4`'s image (the look-alike-stubs probe). Ten `stubtest-*` files
were given codes, dates, flags and `special` 21, the indirect node of `hl2`:

| Patched file | Read by macOS as |
| --- | --- |
| `hlnk`/`hfs+`, `createDate` = private data folder's | a link: content, inode and `nlink` 2 of node 21 |
| same, `createDate` = root folder's (the root had been re-dated) | a link |
| same, `createDate` = the file's own | an ordinary file |
| `hlnk` with creator 0, or type 0 with `hfs+`, either with the private date | ordinary files |
| `hlnk`/`hfs+`, private date, flags `0x0082` (no chain bit) | a link: the chain bit is not part of the file test |
| `fdrp`/`MACS`, flags `0x0022`, `special` 20 (also with `special` 5, without the chain bit, with another date), private date | ordinary files |

The stub test is type `hlnk`, creator `hfs+` and a `createDate` equal to the
private data folder's or the root's. After the root's `createDate` was set
earlier (`SetFile -d`), links made earlier still read as links and a new stub
took the private folder's date, so the dates the test accepts include the
private folder's and the date given to new stubs is only the private
folder's. A Finder alias (`fdrp`/`MACS` with Finder flag `0x8000`, and
`alis`/`MACS`) set through `xattr` is an ordinary file, as is every file whose
Finder information carries `hlnk`/`hfs+` with another date. The `fdrp`/`MACS`
rows cannot show the directory-link test: the target `dir_20` does not exist,
so a passing test and a failed lookup cannot be told apart.

Refusals. `link(2)` on a directory fails with `EPERM`. `link(2)` on a symbolic
link follows it and links the target (`sl/follow` and `sl/target` share an
inode with `nlink` 2); `ln -P`, which links the symbolic link itself, fails with
`Operation not supported`. macOS here makes no hard-linked symbolic link and no
directory link.

Evidence rows. Answered: stub and indirect-node fields; the stub test (codes,
dates, flag) for files; whose metadata macOS reports; link counts; chain
fields; macOS's own attributes on indirect nodes (none); the Finder alias and
`hlnk`/`hfs+` look-alikes read as ordinary files. Open: hard-linked symbolic
links, because macOS cannot make one and the design's rule (a symbolic-link
indirect node) has no native record; it blocks only acceptance of such a node,
which the design already accepts without evidence. Open: the directory
hard-link stub test against Finder aliases, because macOS makes no directory
link and a hand-made stub cannot separate the test from a missing target; it
blocks the design's directory-link detection rule being checked against
native bytes, not the refusal itself.

### E5 Attributes

Status: answered.

Procedure. `capture.rb --populate e5` on `h-hfsplus-16777216-e5` and
`h-hfsx-16777216-e5` (block size 4096; `tools/hfsplus/populate_attributes.rb`),
with `xattr(1)` setting and reading attributes. `fsck_hfs -n` reports both
volumes OK. The attributes tree of both has node size 8192, `attributes`
`0x00000006` (big keys, variable index keys); it holds 16 nodes after the profile.
`fields.rb` decodes its leaf records of all three kinds.

| Question | Observation |
| --- | --- |
| Inline limit | values of 0, 1, 2, 3, 5, 7, 17 bytes and up to 3802 bytes are `0x10` inline records; 3803 bytes and above are `0x20` fork records. Fork records of 3803 to 4000 bytes take one block, 5000 and 8000 bytes two, 70000 bytes 18 (one extent, `750+18`). No `0x30` record appears: the forks hold at most one extent |
| Name order | names compare as UTF-16 code units, unfolded: `com.fsimg.CASE`, `com.fsimg.Case` and `com.fsimg.case` are three attributes in that order, on HFS Plus and on HFSX alike. A 127-unit name, `a:b/c`, `café` and a supplementary character are stored; a 128-unit name fails with `ENAMETOOLONG` (`Errno 63`, the attribute-names probe) |
| ACL | `chmod +a` stores `com.apple.system.Security`, an inline record of 68 bytes for one entry and 92 for two, and sets catalog flag `0x0008` with `0x0004` (`0x008E` on the file, `0x008C` on a folder). The value begins `01 2c c1 6d` |
| Finder information | `xattr -wx com.apple.FinderInfo` creates no attribute record and sets no flag bit (file `0x0082`, folder `0x0080`); `xattr -px` reads the 32 bytes back (`TEXT`, `ttxt`, flags `0x4000` in the file; flags `0x0400` in the folder) |
| Resource fork | writing `FILE/..namedfork/rsrc` fills the record's resource fork (1, 100 and 5000 bytes: one, one and two blocks) with no attribute record; `xattr` lists `com.apple.ResourceFork` as a synthesized name |
| Last attribute removed | flag `0x0004` is cleared with the last attribute: a file with one attribute removed, and a file with two removed, end at `0x0082`; a file with two and one removed keeps `0x0086`. A file that never had one is `0x0082` |
| Symbolic link | `xattr -s` stores an attribute on the link; its record flags are `0x0006` |
| Names macOS adds | only `com.apple.system.Security`, on `.fseventsd`, and `com.apple.decmpfs` on compressed files (E6). Files and folders made by `touch`, `mkdir`, `ln -s`, `ditto` and the hard links of E4 carry no attribute; none carries `com.apple.provenance`, `com.apple.quarantine`, `com.apple.lastuseddate#PS` or `com.apple.system.hfs.firstlink` |

Evidence rows. Answered: attribute name comparison (code-unit order, case
sensitive on both variants); whether flag `0x0004` stays in step (it is cleared
with the last attribute, so verification may require it equal to "the object
has a record"); macOS's own attribute names. Largest inline attribute: 3802
bytes.

### E6 Compression

Status: answered.

Procedure. `capture.rb --populate e6-TOOL` for TOOL `zlib`, `lzvn` and `lzfse`
(`afsctool -c -T CODEC -s 0 -v`) and `ditto` (`ditto --hfsCompression`), each on
HFS Plus (`h-hfsplus-8388608-e6-TOOL`) and `lzfse` also on HFSX
(`h-hfsx-8388608-e6-lzfse`); `e6-sweep` on `h-hfsplus-100663296-e6-sweep` (its
image removed; the fields remain). Content is deterministic text, random bytes
or zeros of 0, 1, 65535, 65536, 65537 and 200000 bytes, text of 131073, 262145,
393217 and 589825 bytes, and a 196608-byte file whose second 65536-byte chunk is
random (`mixed`); `afsctool` also runs with `-L` (`forced-*`). Every manifest
records SHA-256 of the file read through the kernel, which decompresses it, so
a stage 4 or 5 gate compares an extraction with it. `fields.rb` reads the
`com.apple.decmpfs` attribute and the resource fork from the unmounted image
(its `compression` section). `fsck_hfs -n` reports every volume OK.

Header. The attribute is inline (`0x10`), with `magic` bytes `66 70 6d 63`, the
type, and the uncompressed size, as the design states. The catalog record of a
compressed file has `ownerFlags` `0x20`, flags `0x0086` (`0x0004` set), both
fork sizes as below, and the data fork empty (`logicalSize` 0, no blocks) on
every type. Compression adds no other attribute.

Types written, by content and size (`.` none run, `-` left uncompressed):

| Content | zlib | LZVN | LZFSE | ditto |
| --- | --- | --- | --- | --- |
| 0 bytes (any content) | - | - | - | - |
| 1 byte (text, random, zeros) | 3 | - | 11 | - |
| text 65535, 65536 | 4 | 8 | 12 | 8 |
| text 65537, 131073 to 589825 | 4 | - | 12 | 8 |
| text 200000 | 4 | 8 | 12 | 8 |
| zeros 65535, 65536 | 3 | 7 | 11 | 7 |
| zeros 65537 | 4 | - | 12 | 8 |
| zeros 200000 | 4 | 8 | 12 | 8 |
| random 65535 to 200000 | - | - | - | - |
| mixed 196608 | - | 8 | 12 | 8 |

`afsctool` zlib compresses `mixed` only with `-L` (type 4); LZVN and LZFSE
compress it without. `-L` leaves random content uncompressed. `afsctool` LZVN
leaves a file with a one-byte last chunk uncompressed (65537 and the shapes);
`ditto` compresses them. `ditto` writes only types 7 and 8, leaves every text
file of 2000 to 16000 bytes (`fit-ditto-*`) uncompressed, and writes zeros of
65536 bytes inline.

Largest inline size. Every tool writes zeros of 65536 bytes inline (types 3, 7,
11, 7: payloads of 84, 498, 184 and 498 bytes) and 65537 bytes in the resource
fork, at 1, 4, 16 MiB too; no inline file is larger than 65536 bytes. By
payload, `afsctool` stores text inline up to the largest attribute observed,
3612 bytes for zlib (12000 bytes of text), 3247 for LZFSE (10000), 3293 for
LZVN (8000), and moves the next size to the resource fork (16000 bytes of text
zlib, 12000 LZFSE, 10000 LZVN).

Raw markers. A zlib payload or chunk stored raw begins `ff`: the one-byte files
(`ff25`, `ff66`, `ff00`), the one-byte last chunk of 65537 (`ff6f`) and the
random chunk of `forced-mixed`, 65537 bytes long. An LZVN chunk stored raw
begins `06`: the `ditto` last chunk `0668` and its random chunk, 65537 bytes
long (`0660c676`). `afsctool` LZVN compresses the random chunk instead, 66145
bytes. LZFSE has no raw marker: an incompressible chunk or file is an LZFSE
uncompressed block, `62 76 78 2d` (`bvx-`), a count and the bytes, 13 bytes for
one byte (type 11 payload `6276782d 01000000 ...`), or a compressed block that
grew (`bvx2`, 66280 bytes for the random chunk). The largest stored chunk
observed is 66280 bytes.

Type 4 (zlib). The fork begins with a 16-byte big-endian header: data offset
256, map offset, data length and map length 50, then zeros to offset 256. At the
data offset a big-endian resource length (data length minus 4), then a
little-endian chunk count and `count` little-endian `(offset, length)` pairs,
offsets relative to the count. For 200000 bytes of text: data offset 256, 4
chunks, pairs `36+18779`, `18815+18713`, `37528+18722`, `56250+1146`; the first
offset is `4 + 8 * count`. The map, 50 bytes at the map offset, starts with 24
zeros and holds the bytes `66 70 6d 63` (`fpmc`) in its first 32 bytes. Fork size is `256 + data length + 50`
(57706 here). Chunk counts equal `ceil(size / 65536)` in every file.

Types 8 and 12. The fork begins with a table of little-endian words that ends at
the first offset: `afsctool` writes `count + 1` ascending offsets (the first
`8 * count`, the last the fork size) followed by zero words to fill the table to
`8 * count` bytes (1 chunk: `[8, 19458]`; 2: `[16, 19515, 19528, 0]`; 4: five
offsets and three zeros, first offset 32; 7: first offset 56; 10: first offset
80). `ditto` writes no padding: the first offset is `4 * (count + 1)`
(`[16, 25811, 51642, 51644]` for 3 chunks, 44 for 10). Chunk `i` spans
`[offset[i], offset[i + 1])`. LZVN chunks begin with the LZVN opcode bytes
(`e0`, `ef`) and end with `06`; LZFSE chunks begin `bvx2`, the last short chunk
`bvx-`.

Evidence rows. Answered: header, types written per tool, raw markers (zlib
`ff`, LZVN `06`, LZFSE none), type 4, 8 and 12 framings, largest inline size
(65536 uncompressed; the attribute holds up to the observed 3612 bytes), data
fork size (0 on every type).

### E7 Fragmentation

Status: answered.

Procedure. `capture.rb --populate e7` on `h-hfsplus-4194304-e7` and
`h-hfsx-4194304-e7` (block size 4096; `tools/hfsplus/populate_fragmentation.rb`).
One-block files fill the volume until `ENOSPC` (898 files), 60 of the first 120
are deleted (the odd ones), a 20-block `decoy` takes the free runs longer than
one block that remain beside the catalog, and four files write forks into the
holes: `data-9` and `data-17` (data forks of 9 and 17 blocks), `rsrc-9` and
`rsrc-17` (resource forks through `..namedfork/rsrc`). Each reads back with the
checksum written. `fsck_hfs -n` reports both volumes OK; both agree.

| File | Catalog record extents | Overflow records (key: fork, fileID, startBlock) |
| --- | --- | --- |
| `data-9` (ID 920) | 8, one block each (`279+1` ... `293+1`) | data, 920, 8: `295+1` |
| `data-17` (921) | 8 | data, 921, 8: eight extents; data, 921, 16: `330+1` |
| `rsrc-9` (922) | 8 in the resource fork | resource, 922, 8: `348+1` |
| `rsrc-17` (923) | 8 in the resource fork | resource, 923, 8: eight extents; resource, 923, 16: `382+1` |

The extents tree (node size 4096, one leaf of 6 records, depth 1) holds a
76-byte record per eight extents: key length 10, fork type `0x00` (data) or
`0xFF` (resource), fileID, and `startBlock`, the file-relative block where the
record's first extent begins (8, then 16); then eight extent descriptors,
unused ones zero. An overflow record is written only when a fork has more than
eight extents; a fork of 9 extents has one record with one extent. The records
sort by fileID, fork type, `startBlock`.

Evidence row (data and resource forks of at least 9 and 17 extents) answered.
The reader's extents-overflow lookup can be tested against these two images.

### E8 Journal

Status: answered, with one qualification: an interrupted session leaves
transactions only in a copy of the image taken while it runs, never through
`hdiutil detach -force`.

Procedure. Captures `h-hfsplus-33554432-newfs-J-b4096` (E1),
`h-hfsplus-33554432-hdiutil-hfsplusj` (E1) and
`h-hfsplus-33554432-e8-newfs-J-mounted` (`newfs_hfs -J -v J1 -b 4096`, mounted,
unmounted, no file written) give the clean volumes. `tools/hfsplus/e8_interrupt.rb`
formats a fresh 32 MiB `newfs_hfs -J` volume per trial, mounts it, runs a writer that
creates 4096-byte files without syncing until killed, and after a delay either
runs `hdiutil detach -force` (`--mode force`) or copies the backing image while the
writer runs and then force-detaches (`--mode snapshot`); it decodes the journal of
what is left, mounts that image again, decodes once more, and prints one JSON line
per trial. Delays of 0.1, 0.3, 0.6, 1.5 and 3.0 seconds, four trials each, in both
modes: `e8-interrupt-trials/trials.jsonl`. Captures
`h-hfsplus-33554432-e8-interrupted` (first snapshot trial at 1.5 seconds that left
transactions), `h-hfsplus-33554432-e8-remounted` (the same image mounted once more;
fields only) and `h-hfsplus-33554432-e8-journal-disabled` (`diskutil
disableJournal`, see Throwaway probes). The writer stops at 7029 files, which fill the volume, and
most trials from 1.5 seconds on interrupt a full volume. `fsck_hfs -n`
prints `Volume is journaled.  No checking performed.` on every journaled volume, and
the captures' `fsck_populated.passed` is true for that message; the decoder
verifies them.

Journal info block and header on a formatted, never mounted volume: `newfs_hfs -J`
leaves the flags at `0x00000005` (bits 0 and 2), the offset at 12288 (block 3 with
`B` 4096), the size at 524288 and the journal all zero. The first mount by macOS
rewrites the flags to `0x00000001` and writes the header: magic and endian word in
the creating host's byte order (little-endian, `78 4C 4E 4A 78 56 34 12`),
`blhdr_size` 8192, `jhdr_size` 512; the first transaction starts at 512. A volume `hdiutil` journaled has
`start` = `end` = 17408 (E1).

| State | `start`, `end` | `attributes` | Volume header against the catalog |
| --- | --- | --- | --- |
| Formatted by `newfs_hfs -J`, never mounted | header zero, info flags `0x5` | `0x80002100` | Equal (E1) |
| Mounted, `sync`, decoded in place (probe) | 512, 17408 | `0x80002000` | One transaction of 4 blocks, 16896 bytes, not yet written back |
| Cleanly unmounted (`e8-newfs-J-mounted`) | 63488, 63488 | `0x80002100` | `lastMountedVersion` `HFSJ`, `writeCount` 6 |
| `hdiutil detach -force` while writing | equal in 20 of 20 trials (for example 316928), none differ | `0x80002100` in 20 of 20 | A forced detach unmounts cleanly and closes the journal |
| Backing image copied while writing | differ in 10 of 20 trials: none of 8 at 0.1 and 0.3 seconds (still 512, 512), 2 of 4 at 0.6, 4 of 4 at 1.5 and 3.0 | `0x80002000` | Stale (below) |
| The same image mounted again | equal in all 40 remounts (`start` = `end` = 510976 in `e8-remounted`) | `0x80002100` | Replay done |

Rule that separates the states. The journal is a circular log whose header
`start` and `end` are byte offsets into the journal, each at least `jhdr_size`
(512) and below `size`. `start` = `end` means no transaction is pending, at any
position: the clean volumes above sit at 63488, 113664 (the NPS `hfsjtest1`
specimens), 159744, 316928 and so on. `start` is not equal to `end` when
transactions are pending, and `start` may be greater than `end` once the log has
wrapped (207872 and 144384, 334336 and 270848 in `trials.jsonl`). A volume never
mounted since `newfs_hfs -J` has no header at all (info flags bit 2 set, header
zero) and holds no transactions.

What a pending transaction looks like in `e8-interrupted` (`start` 512, `end`
460800, `size` 524288): at the journal offset plus `start` is a block list header
of two 16-bit counts (`max_blocks` 511, `num_blocks` 54), a 32-bit `bytes_used`
(229888), a checksum word and a pad word, then 16-byte block infos of a 64-bit
block number, a 32-bit size and a 32-bit word that differs per entry: entry 0
holds block 0, size 0; the next ones block 7200 and 7192 with size 4096 and block 2
with size 512. `max_blocks` is `(blhdr_size - 16) / 16`: 511 for 8192, 1023 for 16384 (the NPS
specimens). The block numbers count 512-byte units: the two 4096-byte entries are
8 apart and the 512-byte entry is block 2, the volume header at byte 1024. The volume header of the interrupted image says `fileCount` 1135 and
the catalog holds 1135 files; after the second mount (replay) both say 1137, so the
base structures lag the journal while `start` is not equal to `end`, and
`attributes` bit 8 (`unmounted`) is clear.

Journal files. On a `newfs_hfs -J` volume the root holds `.journal` (ID 16, type
`jrnl`, creator `hfs+`, flags `0x0002`, mode `0100000`, owner 0, data fork 524288
bytes in extent `3+128`) and `.journal_info_block` (ID 17, data fork 4096 bytes in
extent `2+1`); after the first mount the private folders are IDs 18 and 19 and
`.fseventsd` follows (ID 20), as in E2. Both journal files stay after unmounting
and after an interrupted session. `diskutil disableJournal MNT` works unprivileged
on a volume `hdiutil` mounted: it clears `attributes` bit 13 (`0x80002100` becomes
`0x80000100`), removes `.journal` and `.journal_info_block` from the catalog (the
root then holds only the user's file, the private folders and `.fseventsd`; file
count 4, next catalog ID 25), frees their blocks (against `e8-newfs-J-mounted` the first clear
allocation block moves from 259 to 2 and the set bits fall from 327 to 199), leaves
`journalInfoBlock` at 2 and sets `lastMountedVersion` to `10.0`. `fsck_hfs -n` then
checks the volume and reports it OK.

The NPS `hfsjtest1` specimens, read with `fields.rb` and never mounted (10 MiB bare
volumes, 4096-byte blocks, `attributes` `0x2100`: bit 31 clear, as on a volume
unlike every volume of E1 and E2): both have info flags `0x1`, offset
12288, size 8388608, little-endian header, `blhdr_size` 16384, `jhdr_size` 512 and
`start` = `end` (113664 in gen0, 159744 in gen1). gen0's journal holds 163 nonzero
sectors of 16384 and gen1's 240, among them a block list header at byte 512
(gen0: `max_blocks` 1023, `num_blocks` 7, `bytes_used` 37888), consistent with
residue of completed transactions. A reader that trusts only `start` and
`end` reads both as clean.

Evidence rows answered: journal info block flags from `newfs_hfs -J` (`0x5`) and
`hdiutil` (`0x1`), the header's byte order (little-endian on macOS hosts), the
journal files' presence and forks, the state after an interrupted session, and the
files after journaling is disabled. Not established: a pending transaction from
`hdiutil detach -force` (20 of 20 force trials left none), so only the image-copy
procedure, which reproduces what a crash leaves, produces `start` not equal to
`end` natively; its yield depends on timing (0 of 8 under 0.6 seconds, 10 of 12 at
0.6 seconds and later, and the four 3.0-second trials are identical copies of one
state).

### E9 Large trees

Status: answered.

Procedure. `newfs_hfs -v E9 -b 4096 -n c=4096 -c c=76800` on a 640 MiB image
(catalog clump of 76800 blocks, 300 MiB, so the catalog tree starts with 76800
nodes of 4096 bytes and its node map needs map nodes), then four captures, each the
image of the one before it copied and mounted again
(`tools/hfsplus/populate_large.rb`): `h-hfsplus-671088640-e9-create` (50
directories of 2000 empty files each, one directory level, 100000 files; every
seventh name carries one to six extra characters), `e9-delete` (every third file of
every directory, all of `d07` and the files 500 to 1499 of `d20`: 35350 files),
`e9-thin` (every file whose number is not a multiple of 50) and `e9-sparse` (every
file whose number is not a multiple of 997). Each stage's `fsck_hfs -n` reports the
volume OK. Creating 3000 empty files took 0.52 seconds in a probe (Throwaway probes), and
the 100000-file capture, formatting and checks included, ran from 22:50 to 22:51
by its file times, so the files are made with plain `File.write` calls. The images and manifests are kept gzipped (`image.img.gz`,
`manifest.json.gz`, about 6 MiB per capture); `fields-index.json` beside each
`fields.json` is `fields.rb --json --index --records 5` of the image.

| Stage | Files | Catalog leaf records | Leaf nodes | Depth | Root records | Level 2 index nodes (records each) |
| --- | --- | --- | --- | --- | --- | --- |
| `e9-create` | 100006 | 200120 | 8203 | 3 | 65 | 65 (61 to 132) |
| `e9-delete` | 64659 | 129426 | 7960 | 3 | 64 | 64 (61 to 132) |
| `e9-thin` | 1271 | 2650 | 2283 | 3 | 64 | 64 (13 to 50) |
| `e9-sparse` | 13 | 134 | 56 | 3 | 52 | 52 (1 to 2) |

Catalog records are a file or folder record and its thread record each (200120 =
2 * (100006 + 54)). The catalog fork is 314572800 bytes in one extent
(`15366+76800`), 76800 nodes, and `fork_size_matches_nodes` is true.

Map nodes. The catalog's header node holds 3840 map bytes (`4096 - 256`, 30720
bits), so 76800 nodes need map nodes: the header's `fLink` is node 2, and nodes 2
and 3 are map nodes (kind 2, height 0, one record, `bLink` 0 on both, node 2's
`fLink` 3, node 3's `fLink` 0). Each map record is 4076 bytes (`4096 - 20`) at
offset 14, so its record offsets are `[14, 4090]` and two bytes lie between the end
of the record and the 4-byte offset table at 4092. The three maps cover 95936 bits
(`30720 + 2 * 32608`); bits at and beyond 76800 are zero; the map nodes themselves
are marked in use (the formatted volume has 4 bits set and the first clear bit is 4:
nodes 0 to 3). The record length was
observed at node size 4096 only: `-n e=1024 -I e=1700` and `-n e=2048 -I e=3400`
leave the extents tree at 512 and 256 nodes, and an 8192-byte catalog tree needs
more than 63488 nodes (about 500 MiB) to have map nodes, which was not tried.

Index keys. Every index record in the 249 index nodes of the four stages was
compared with the first key of its child (8203, 7960, 2283 and 56 leaf children
at level 2 and the level 2 nodes under each root): an index record's key equals the
first key of the child it names in every stage (`key_mismatches` 0) and the child's
height is one less. After the deletions this is not trivial: 2343 of the 7960 level-2
keys of `e9-delete` are not keys of `e9-create`, so macOS rewrites an index key when
its child's first record is deleted.

One-record non-root index nodes. `e9-sparse` has 48 of them (nodes 134, 266, 397,
527, 788, ...) among 52 level-2 nodes, below a root of 52 records, and `fsck_hfs -n`
reports the volume OK. The native tree gets there by deleting records only: leaf
nodes that still hold a record are never merged (2283 leaf nodes hold 2650 records
in `e9-thin`), nodes left empty are freed, and the root is never collapsed (depth
stays 3 with 134 leaf records). The three earlier stages have none.

Evidence rows answered: map-node record length (`M - 20`, observed at `M` 4096);
index keys equal to their child's first key after native deletions (yes); whether
`fsck_hfs` accepts a one-record non-root index node (it accepts the 48 in
`e9-sparse`, which macOS itself made), so the packer needs no rule against them.
Not shown: that `fsck_hfs` accepts such a node as the *last* node of a level (the
specimens' positions were not checked against the level ends).

Native quirk found here: a volume formatted with a 76800-block catalog clump has
`nextAllocation` 860166, beyond its 163840 blocks; `fsck_hfs -n` on the unmounted
formatted volume reports `Volume header needs minor repair` (exit 8), and the
first mount rewrites the field (2585 after the population). Setting the field to
92166 by hand makes `fsck_hfs` report OK (Throwaway probes).

### E10 Old volumes

Status: open. It needs Mac OS 9 and early Mac OS X images, which the owner has not
supplied; no native tool on this host produces them. It blocks the evidence row on
names and BSD information on those volumes (reader tables and name rules).

### E11 Wrapper

Status: answered.

Procedure. Linux, in the container of `build-aux/linux-check.sh` (`-r` a committed
revision, the command `truncate -s SIZE img; mkfs.hfsplus ... img; fsck.hfsplus -n
img`, the image passed out as gzip and base64). `mkfs.hfsplus` has `-w` ("add a HFS wrapper") and `-h` (a plain HFS
volume); macOS `newfs_hfs` rejects `-w` (`illegal option`). Captures
`h-hfsplus-16777216-e11-mkfs-w`, `h-hfsplus-67108864-e11-mkfs-w`,
`h-hfsplus-16777216-e11-mkfs-w-b512` (`-w -b 512`), `h-hfsplus-16777216-e11-mkfs-w-J`
(`-w -J`), `h-hfsplus-16777216-e11-mkfs-h-classic` (`-h`) and, for the macOS mount of
the journaled one, `h-hfsplus-16777216-e11-mkfs-w-J-mounted`. The archival specimen
is `h-hfsplus-9713664-e11-blender-2.28`: partition `2.hfs` of
`blender-2.28-darwin-6.6-powerpc.dmg`, extracted statically with `7zz e -tdmg -so`
(the dmg is not attached) and decoded with `fields.rb`; its `commands.txt` gives
the checksum. `fields.rb` decodes the outer master directory block, the outer
volume bitmap and the outer extents file's leaf records.

| Field | Blender 2.28 | `mkfs.hfsplus -w` (16 MiB and 64 MiB) | `-w -b 512` |
| --- | --- | --- | --- |
| Image size | 9713664 | 16777216, 67108864 | 16777216 |
| `drAlBlkSiz` | 4096 | 4096 | 512 |
| `drAlBlSt` (512-byte sectors) | 8 | 8 | 11 |
| Embed signature, start, count | `H+`, 10, 2360 | `H+`, 10, 4084 and 16372 | `H+`, 56, 32699 |
| Embedded volume offset | 45056 | 45056 | 34304 |
| `drNmAlBlks` | 2370 | 4094 and 16382 | 32755 |
| `drFreeBks`, outer bitmap | 0, every block set | 0, every block set | 0, every block set |
| `drAtrb` | `0x8300` | `0x8300` | `0x8300` |
| `drNmFls`, `drNxtCNID` | 5, 21 | 5, 21 | 5, 21 |
| Outer extents file | 4096 bytes, `0+1` | same | 1024 bytes, `0+2` |
| Outer catalog file | 4096 bytes, `1+1` | same | 2048 bytes, `2+4` |
| Outer extents-tree leaf record | file 5 data from 0: `10+2360` | `10+4084`, `10+16372` | `56+32699` |
| Embedded `H+` | version 4, 4096-byte blocks, 2360 blocks | version 4, 4096, 4084 and 16372 | version 4, 512, 32699 |
| Volume name | `untitled` | `untitled` (`-v WRAP` names the embedded volume) | same |

Layout. Sectors 0 and 1 of the image are zero; the master directory block is at
byte 1024 (`BD`); the outer bitmap starts at sector 3; the first allocation block
starts at sector `drAlBlSt` (byte 4096 for `drAlBlSt` 8). Outer block 0 is the
extents file, block 1 the catalog file, blocks 2 to 9 hold nothing the decoder
reads, and blocks 10 to the end are the embedded volume: `embed_count` equals the
embedded `totalBlocks` and the two block sizes are equal. The embedded volume's own
bytes 0 to 1023 are zero, its volume header is at its byte 1024 (image byte 46080),
and its alternate header is `embed_count * block_size - 1024` bytes into it (image
byte 9,665,536 + 45056 = 9,710,592 in Blender, `0x942C00`). The alternate master
directory block is at `image_size - 1024` (`0x943400` in Blender, signature `BD`),
in the 2048 bytes that follow the last outer allocation block. Allocation block
numbers and fork extents inside the embedded volume, including `journalInfoBlock`,
count from the embedded start; the journal info block's byte `offset` is the
exception (below).

Bad block file. The outer extents file is a one-leaf HFS B-tree of 512-byte nodes
whose only record is the key (file ID 5, data fork, start block 0) with the extent
`10+2360`: the bad block file covers exactly the embedded range, and the outer volume
bitmap sets every block. `drAtrb` `0x8300` has bits 15, 9 and 8 set. The outer
volume holds five files (`drNmFls`) that the decoder does not list.

Journaled wrapper. `mkfs.hfsplus -w -J` writes the journal info block's offset
(12288) relative to the embedded volume, with flags `0x5` and no header. After macOS
mounts the volume (it does mount the wrapped volume, writable and journaled) the
offset reads 57344 = 45056 + 12288 and the journal header (`78 4C 4E 4A`) is at image
byte 57344: the offset then counts from the start of the image, not from the
embedded volume. `fields.rb` reports `offset_counts_from: image` for that case.

Evidence row answered: wrapper layout. The macOS side of a wrapper cannot be made
natively here (no tool formats one), so wrapped volumes come from `mkfs.hfsplus -w`
and the one archival specimen, which agree in every field above except the
geometry.

