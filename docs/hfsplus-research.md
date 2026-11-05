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
| Apple open-source `hfs` at tag `hfs-715.120.4` (commit `124b970fd5390f1c2cd6f985478616a04f6eeb1f`, <https://github.com/apple-oss-distributions/hfs>) and `xnu` at tag `xnu-12377.121.6` (commit `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`, <https://github.com/apple-oss-distributions/xnu>), read-only, outside the tree | APSL 2.0; facts only, paraphrased, no code, table, declaration or identifier enters the repository; tables are compared by a throwaway script and only the result is recorded | The [Apple source cross-check](#apple-source-cross-check) |
| NetBSD `sys/fs/hfs/libhfs.c` | BSD-2-Clause | A second, independent derivation of the folding table, compared, not imported |
| Unicode 3.2.0 character data | Unicode license, notice in `third-party/Unicode-LICENSE.txt` | Input of the table generator |
| Apple `lzfse` at a pinned commit | BSD-3-Clause | The imported LZFSE and LZVN decoders, under the [codec design](../specs/design-codecs.md#imported-lzfse-and-lzvn-decoders) |
| macOS `hdiutil`, `newfs_hfs`, `fsck_hfs`, `diskutil`, `ditto`, `xattr` | Native tools on the development host and the CI runner | Captures and native qualification |
| `afsctool` (Homebrew) | GPL-3.0-only and BSL-1.0, external tool | Compressed files of each chosen type |
| `lzfse` command (Homebrew) | BSD-3-Clause, external tool | Test vectors for the LZFSE decoder |
| hfsprogs `mkfs.hfsplus` and `fsck.hfsplus` | APSL-derived binaries, external | Linux formatter and checker oracle |
| hfsfuse `hfsdump`, built from a pinned revision outside the tree | MIT and bundled terms, external | Independent reader |
| The Sleuth Kit `fls`, `istat`, `icat` | Mixed licenses, external | Independent forensic reader |
| Linux kernel `hfsplus` | GPL-2.0, reference only | No harness compares against it; a mount comparison is [open work](todo-hfsplus.md). No code, declarations or identifiers enter the tree |

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

Project-owned Ruby tools produce and read captures. `tools/hfsplus/capture.rb`
formats a volume with a native tool (or imports an image another tool
made), optionally mounts it and populates it, unmounts it, runs `fsck_hfs -fn`
(a forced, read-only check; the result is `passed`, `failed` or `skipped`),
and writes `commands.txt`, `manifest.json`, `fields-formatted.json` and
`fields.json` beside the image; it detaches everything it attached, on failure
too. `tools/hfsplus/fields.rb IMAGE` decodes an image's boot area, volume
header and alternate header, tree header nodes, node maps, allocation bitmap,
private folders, Finder information and catalog records as text or JSON
(`--json`), treating the image as untrusted; it also decodes the attributes and
extents overflow leaf records and, for compressed files, the `decmpfs` header and
the resource fork's chunk table; `--index` walks the index nodes of each tree and
compares every index key with the first key of the child it names. `capture.rb
--populate` takes the profiles `e2`, `e4`, `e4-dirlinks`, `e5`, `e6-zlib`, `e6-lzvn`,
`e6-lzfse`, `e6-ditto`, `e6-sweep`, `e7`, `e9-create`, `e9-delete`, `e9-thin` and
`e9-sparse` (`tools/hfsplus/populate*.rb`); `tools/hfsplus/patch_record.rb`
rewrites fields of one catalog file record in a copy of an image, and
`tools/hfsplus/e8_interrupt.rb` interrupts mounted journaled sessions and decodes
the journal that is left.
`tools/hfsplus/unicode-probe.rb` runs [E3](#e3-unicode) and writes its tables.

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

* Look-alike stubs (E4). Read the `createDate` of the root folder (ID 2) and of
   the private data folder (ID 16) from `h-hfsplus-8388608-e4/fields.json`, and
   patch ten of the `stubtest-*` files of `h-hfsplus-8388608-e4/image.img`.
   `patch_record.rb SRC DEST NAME` reads SRC, writes a new file DEST and
   refuses a DEST that exists, is SRC or lies under `~/hfsplus-captures`, so the
   ten patches chain: the first reads the capture's image and writes `V.1`, each
   later one reads the previous copy and writes the next, up to `V.10`. The
   commands are `ruby tools/hfsplus/patch_record.rb SRC DEST stubtest-a --type
   hlnk --creator hfs+ --create-date PRIVATE --special 21 --flags 0x22` for `a`,
   with `--create-date ROOT` for `b`, no `--create-date` for `c`, `--creator
   0x00000000` for `d`, `--type 0x00000000` for `e`, no `--flags` for `f`, and
   `--type fdrp --creator MACS --special 20 --flags 0x22` for `g` (`h` without
   `--create-date`, `i` without `--flags`, `j` with `--special 5`). Mount
   `V.10` writable and run `stat -f 'ino=%i nlink=%l size=%z' stubtest-X; cat
   stubtest-X` on each. `ROOT` is the date after the profile's `SetFile -d`
   step; the run shows the private date and the root date both read as links.
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
LZFSE streams (`lzfse -encode -i IN -o OUT`) and supplies LZFSE test
vectors.

Probes of E3, kept as a capture directory. `~/hfsplus-captures/e3-small-names/`
holds `commands.txt` and the tables of `ruby tools/hfsplus/unicode-probe.rb
--small-names`: one 8 MiB volume per name, formatted with `newfs_hfs -v E3 -b
4096` (plus `-s`), mounted with `hdiutil attach -imagekey
diskimage-class=CRawDiskImage -mountpoint MNT V`, holding the one name
(`O_CREAT | O_EXCL`), detached, and checked with `fsck_hfs -d -n` on the raw
device. `fsck.tsv` has the stored units and verdict of each name; the tool reads
the units from the catalog leaves. `class0-VARIANT.tsv` holds one volume with
the names that probe marks decomposing from class 0 values.
`marks-VARIANT.tsv` holds one more volume per comparator with 1003 names of
longer runs of marks and marks after precomposed starters, `marks-fsck.tsv` its
`fsck_hfs -d -n` verdict, and `marks-commands.txt` its commands. The tool
refuses to replace an existing directory without `--overwrite`; the `marks`
files were added to a capture whose other files are byte for byte the first
run's, so `commands.txt` is that run's. `fsck-sequences.tsv` was added the same
way, with `fsck-sequences-commands.txt`: one 8 MiB volume per name and
comparator, with the stored units, the exit status of `fsck_hfs -d -n` and the
illegal and replacement names it prints.

SHA-256, `e3-small-names/`:

```
7fa94662aadf1d7db55a68e6f77c770a4c03181652cfb22a4b6e078e530c23d1  class0-hfsplus-differences.tsv
62389a69ef25443b2af30dcc169fd656eb5ec5d17bda9429fbafd06789ed0d63  class0-hfsplus.tsv
7fa94662aadf1d7db55a68e6f77c770a4c03181652cfb22a4b6e078e530c23d1  class0-hfsx-differences.tsv
62389a69ef25443b2af30dcc169fd656eb5ec5d17bda9429fbafd06789ed0d63  class0-hfsx.tsv
7250642374cc13b8bf824fd99dd18e1c6f20c5fc4b3568096fcd24d1a5fedc63  commands.txt
378acf247e6e394b717ff001b7f6153596d2a1c63b59da0fe08d8fe9a7b63a3d  fsck-sequences-commands.txt
00faaa45ba9b9d45fcc8d8da087c9fc5bb98d8c7ab688f74c27ce8ed219016a0  fsck-sequences.tsv
bfa659d23b2274101e693efd5f38ebd9372e725c136ce00139d6221a2b84fcb4  fsck.tsv
c0af88516fa54e91de78525743714008679be2dc3e49c2b6b4ce01ce5d9ad03e  marks-commands.txt
6b6f09e9d5ac192184d4405cb94e7d94ce2251f00317f7a6a032c77f55e8d4bb  marks-fsck.tsv
49527f41182a766790970ac794a440c80e8b5fc8bf5d150d3e1498ed225a5eee  marks-hfsplus.tsv
8d35dd4d6468410e349e7e67029af161cd30e7c58d9868aec4b5ea144095d50f  marks-hfsx.tsv
```

## Experiments

Each experiment varies one property on disposable volumes, as the plan's stage
1 table defines. Each section gives its status, the procedure
actually run, the observations and the table or field each consumer needs.

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
volume only as journaled and not checked. `fsck_hfs -fn` forces the read-only
check of a journaled volume, and `capture.rb` runs it (the captures taken before
that change ran `fsck_hfs -n`: their `fsck_populated.passed` is true on a
journaled volume although nothing was checked, which the new
`fsck_populated.result` records as `skipped` when the message appears). The
decoder reports the volume's consistency fields; it verifies no journal
checksum.

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
| Clump sizes | `dataClumpSize` = `rsrcClumpSize` = `min(16 * B, 65536)`; the extents and catalog forks' `clumpSize` equals their `logicalSize`, and each B-tree header's `clumpSize` equals its fork's (`h-hfsplus-1048576-newfs-b512` and `-b4096`); see the tree table |
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
  clear everywhere. [E10](#e10-old-volumes) gives old volumes.
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
finds no problem on any of them. On the journaled ones the earlier captures hold
no native verdict (see above).

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
| 1904-01-01 (file, folder) | `createDate` 0 and `contentModDate` 0 (the creation date is lowered to the modification date); access 0; attributeMod is the write time |
| 1969 (file) | create, contentMod and access all 2065844800; attributeMod is the write time |
| 1969 (symbolic link, `lutime`) | create, contentMod and access all 2065844800 |
| 2040-02-06 06:28:15 | contentMod and access 4294967295; create is the write time |
| 2222222222 Unix | contentMod and access 10099726: the value is reduced modulo 2^32 and reads back as a 1904 date. While mounted, `lstat` reports the unreduced 2222222222 (recorded by the step's `output` in `manifest.json`) |

These rows come from the captures `h-*-e2-unread-*`, in which nothing reads the
tree before `fields.json` is decoded: `capture.rb` lists and checksums the tree
through a second, read-only mount afterwards. The earlier E2 captures walked and
checksummed the tree while it was still mounted, and in them every file's and
folder's access date is the write time, which the read caused (so those captures
cannot show an access date). Two probes on a throwaway 8 MiB volume, with a file
`f`, a folder `d` and a link `l` set by `utimes` to access 1500000000 and
modification 1600000000, unmounted and decoded: with no read before the unmount
`f` and `d` store access 1500000000 (the value set) and `l` too; with a read
of `f` and a listing of `d` (also after a two-second pause) `f` and `d` store the
time of the read and `attributeMod` moves with it, while `l`, which was not
read, keeps 1500000000. A regular file's and a folder's access date therefore
follows `utimes` and is replaced by a read; the earlier conclusion that it
ignores `utimes` was the read's effect. `backupDate` is 0 everywhere.

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

Observed, not established as a policy: after the population session only the
geometry and the five fork-data records matched among the fields that change, and the
alternate had not seen the session's counts, dates or `writeCount`; after the one
case of catalog growth the alternate held the catalog fork of that moment, with
the counts and `attributes` of that moment too (clean bit clear). Whether the
alternate is rewritten at other events (a clean unmount, a `sync`, the fork
growth of other special files) was not tried, and no vendor documentation is
cited here for the update rule. The fields observed equal in these two cases are
the five fork-data records and the geometry; the observation does not show that
they are always equal.

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
  size, total blocks and the five forks where the two headers were observed to
  agree, and should not compare `attributes`; the update rule is not established.
* Volume attribute bits and the other flags on native volumes after use: the
  clean bit clears while mounted and returns on unmount; bit 31 is always set;
  no bit outside the design's table appears. Catalog flags seen: `0x0002`,
  `0x0004`, `0x0008`, `0x0010`, `0x0080`; no `0x0020`, `0x0040` or `0x1000`.
  The B-tree flags stay as formatted, with the bad-close bit clear.
  [E10](#e10-old-volumes) answers this in part for older volumes.
* Longest symbolic-link target macOS stores and the Finder codes it writes:
  1023 bytes; `slnk`/`rhap`.
* Whether `newfs_hfs` sets folder counts: E1 and the folder row above (HFSX
  keeps them current; HFS Plus does not set the flag on user folders).
* Rows left open here because the experiment that feeds them is another:
  the map-node record length (E9), the Unicode rows (E3), the hard-link,
  attribute, compression and journal rows (E4 to E8).

### E3 Unicode

Status: answered, on `newfs_hfs`'s two comparators (`H+` with compare type
`0xCF` and `HX` with `0xBC`), for every value a POSIX name can carry. Every
observed row is explained by Unicode 3.2.0 plus the rules below. Not observable:
the fold rows of units no POSIX name can store, which the table sets by rule, and
the compare types `newfs_hfs` cannot make (see "What cannot be observed").

| Element | Answer |
|---|---|
| Fold table | `fold.tsv`, per unit, on `H+`. Of the 443 units on the folded pages that have a Unicode 3.2.0 lowercase, 304 are observed: they fold to that lowercase, except 30 that stay themselves. The other 139 (from U+00C0 on) decompose, so no stored name holds them and their fold is not observed. The table's rule is lowercase on the folded pages unless the unit decomposes, so those 139 stay themselves, and it equals NetBSD's table on all 65536 units |
| Ignorable units | U+200C to U+200F, U+202A to U+202E, U+206A to U+206F and U+FEFF on `H+`; none on `HX` |
| U+0000 ordering | Reachable: the POSIX name U+2400 stores U+0000. It sorts after U+FFFD, the last storable unit, on `H+`, and first on `HX` |
| Decomposition table | The prediction holds for every BMP value, including all of Hangul; no supplementary value decomposes |
| Combining order | Stable sort by Unicode 3.2.0 class of each maximal run of nonzero class, after full canonical decomposition, for every ordered pair of the 297 BMP marks after `a`, for the three class 0 values that decompose into marks, and for 1003 names of longer runs and marks after precomposed starters; the 30 supplementary marks behave as class 0 |
| Supplementary characters | Stored as their surrogate pair, compared unit by unit, never folded or decomposed, and never reordered in the two-mark runs probed |

Procedure. `tools/hfsplus/unicode-probe.rb` formats one disposable 8 GiB sparse
volume per comparator with `newfs_hfs -v E3 -b 4096 -c c=262144` (plus `-s`
for `HX`), mounts it, and creates one empty file per name with `O_CREAT |
O_EXCL`. A name that collides with an earlier one is retried in the next
directory (`SET.0`, `SET.1`, and so on) until it is created, so that a
class's members all show their stored form. A collision names its
representative by the inode the colliding name resolves to. The tool reads each
directory with `find -print0`, because Ruby's `Dir` composes the names it reads
from an HFS volume and returns spellings the system cannot find again. After
unmounting it runs `fsck_hfs -d -n` and walks the catalog leaf chain through
`fields.rb`: the leaves hold the stored units and the sort order. It then
removes the two names `fsck_hfs` calls illegal and runs `fsck_hfs` again. Each
image allocates 1.02 GiB and is deleted at the end. A run takes about three
minutes per volume, of which the names are 53 seconds on `H+` and 92 on `HX`:
creating and listing a name costs 86 and 150 microseconds from Ruby by
`volume.json`, so no helper is needed. The run stops, writing no table, if
`readdir` and the catalog disagree on a directory's names or count, if the leaf
walk does not find the record count of the tree header, if a created name has no
leaf, or if `fsck_hfs -n` still fails once its two illegal names are removed. A
rerun refuses an existing capture directory without `--overwrite`;
`--analyze-only` rewrites only the derived tables of the kept `SET.tsv` files.

Input. `UnicodeData-3.2.0.txt` from
<https://www.unicode.org/Public/3.2-Update/UnicodeData-3.2.0.txt>, in
`~/hfsplus-captures/unicode-3.2.0/`, SHA-256
`5e444028b6e76d96f9dc509609c5e3222bf609056f35e5fcde7e6fb8a58cd446`. The tool
refuses any other digest.

Names.

| Set | Names | `H+` created | `H+` `EEXIST` | `HX` created | `HX` `EEXIST` | Other |
|---|---|---|---|---|---|---|
| `alone`: the value | 241311 | 240588 | 722 | 241282 | 28 | U+0000 |
| `between`: `aXb`, after the seed `ab` | 241312 | 240589 | 721 | 241284 | 26 | U+0000; `/` gives `ENOENT` |
| `pairs`: `a` and two marks, all 327 squared | 106929 | 71506 | 35423 | 71506 | 35423 | |
| `hangul`: each syllable, then its jamo | 22344 | 11172 | 11172 | 11172 | 11172 | |
| `surrogates`: the 2048 three-byte encodings | 2048 | 2048 | 0 | 2048 | 0 | |

`alone` and `between` hold all 63488 BMP scalar values, the 176012
supplementary values Unicode 3.2 assigns (including the private use planes),
and 1811 unassigned supplementary values that reach every surrogate unit no
assigned value reaches. Each catalog holds about 614000 files, in 43 folders on `H+` (compare type
`0xCF`) and 14 on `HX` (compare type `0xBC`).

Tables, in `~/hfsplus-captures/e3-hfsplus/` and `e3-hfsx/`, tab separated with
one `#` header line, about 41 MB per comparator:

* `SET.tsv`: `id` (hex value, `+` between the values of a pair or jamo
  sequence), `scope` (`bmp`, `supp`, `coverage`, `seed`, `pair`, `hangul`,
  `hangul-jamo`, `surrogate`), `disposition` (`created`, an errno name, or
  `path-terminator` for U+0000, which no syscall can carry), `representative`
  (`self`, the `id` it collided with, `dir` for its own directory, `-`),
  `pass` (the `SET.N` holding the name), `readdir_units`, `leaf_units`
  (`.`-separated UTF-16 units) and `rank` (0-based among the directory's
  catalog children).
* `fold.tsv`: `unit`, `status`, `fold`, `chain`, one row for each of the 65536
  table indexes. A collision class is the set of stored units whose `aXb`
  names collide; the classes sit on a chain in catalog order, and `chain` is a
  class's 0-based position on it (`-` for units not on it; U+0000's class is
  left off, see `position`). `determined`: `fold` is the only member of the
  unit's class that catalog order allows. A class with one stored member is
  determined by an axiom of the analysis, that a class's fold is one of its
  stored members; U+0000 shows the axiom is false in general, and for a unit
  such as U+212B (below) the row is evidence of position on the chain only.
  `ambiguous`: `fold` lists the members of the class that some ascending choice
  along the chain allows (`|` separated). The members are interchangeable only
  jointly, through a monotone choice along the chain, not one class at a time:
  for 47 of the 124 ambiguous classes a storable unit lies between two members
  (`0410|0430` has U+0411 between them, `2160|2170` has U+2161), and choosing
  U+0430 for the first forces U+0431 for `0411|0431`. `ignorable`: `aXb`
  collides with `ab`, `fold` is 0000. `position`: U+0000, with the unit it
  sorts after (the largest member of the class before it) or `first`.
  `pair-only`: the unit occurs in stored names only inside a surrogate pair;
  `fold` is its group, `high` (D800 to DBFF) or `low` (DC00 to DFFF), and the
  observation is order within a group, not a value (see "Supplementary
  characters"). `unobservable`: no name stores this unit, and `fold` says why:
  `decomposes`, `escaped` or `system-mapped`.

  A table agrees with `fold.tsv` when (1) all units with one `chain` value get
  one fold; (2) that fold is the `fold` of a determined row or one of the listed
  values of an ambiguous one; and (3) the folds of successive chain positions
  strictly ascend. Testing each ambiguous row for membership alone is not
  enough, because it accepts choices that break rule 3. For `pair-only` rows
  the table's folds strictly ascend over D800 to DBFF and over DC00 to DFFF in
  unit order. Rows of the other statuses constrain nothing beyond their own
  value.
* `differences.tsv`: `set`, `id`, `check`, `predicted`, `observed`: every
  disagreement with `specs/design-hfsplus.md` as written, computed from the
  pinned data, with the joint rule of `fold.tsv` as check `chain`.
  `residual.tsv`: the same after adding the rules of this section.
* `volume.json` (tree shape, both `fsck_hfs` verdicts, per-set directories),
  `summary.json` (these counts), `commands.txt` and `SHA256SUMS`.

SHA-256, `e3-hfsplus/`:

```
546d049928ee2557ef7ff0add83af7ff53b21db8d7963e241a43accf1a6047df  alone.tsv
03ce83ec5be728278c483d8ca8a02823cb136dc6ff2b9ddb2ac73604743c058b  between.tsv
5496255e6669edeef6daff50f59987c4dfbb542314f4acd97f085293ea90f238  commands.txt
387381fd16cdf084644791a6dd04592444049f267c71c7e9a2e43906a7c112e0  differences.tsv
ea56a78b3df351a9d4a2cc76df369d9ab14b6e9d3ff1c5c9a187b3250ad9164e  fold.tsv
577705e9a730b2d105b7bcc708ad2de8edad8163a08c5036c4ca7c90beb828ec  hangul.tsv
81b2d03dd3230dbdb184eb5aa61d1c48989f684bcc6f657d3f36d2de860e23f5  pairs.tsv
2fac0f49e5d1c1b2e98708a23bf5f65cd80d9b23d73bae48b6283991c581406e  residual.tsv
cab219a52250167dfa3a839dbdad7ec7b49318bf8eb24d0447806ec1d3b54380  summary.json
db3cecc55bdd07ddd476155276ee87ab3373f61798931eb3768d36a1c0d2c36c  surrogates.tsv
ff03becd2b89a95a57b05f1e994e3269602e57a4ec44383d40702fc3206e782d  volume.json
```

SHA-256, `e3-hfsx/`:

```
54935d68c141ef5a7a3f377c7eac2974aee9fd9880fc9a34f9313794d9c5636d  alone.tsv
74ff25021ba5e0abf51778b991ed4eb1c32f9fdbbb425664f84240b44b4e9d66  between.tsv
a21f2f48e4d55b8a8f193e177d0accaa82fb7681e35c5a5654dacb22ca89eb55  commands.txt
350851895351025e5f36c0717da0ddf314bb44dd1dccdc4b5a0608032f92761c  differences.tsv
375703e87e5e7e185d20b8fc8c515de7badecee268a66917544fbe4cd6eff802  fold.tsv
577705e9a730b2d105b7bcc708ad2de8edad8163a08c5036c4ca7c90beb828ec  hangul.tsv
81b2d03dd3230dbdb184eb5aa61d1c48989f684bcc6f657d3f36d2de860e23f5  pairs.tsv
2fac0f49e5d1c1b2e98708a23bf5f65cd80d9b23d73bae48b6283991c581406e  residual.tsv
30517effff381619ecd5586b436046c89bea1c20a6daf79b940934fc156efb85  summary.json
db3cecc55bdd07ddd476155276ee87ab3373f61798931eb3768d36a1c0d2c36c  surrogates.tsv
91da3c9072a8f0127916f4b5ec4b9a133009d89d99b6d7e032f468fa04b1b53b  volume.json
```

`fold.tsv` on `H+` holds 51073 `determined`, 252 `ambiguous` (124 classes), 16
`ignorable`, 1 `position`, 2048 `pair-only` and 12146 `unobservable` rows; on
`HX`, 51341 `determined`, 1 `position`, 2048 `pair-only` and 12146
`unobservable`.

Explanation. `residual.tsv` holds 5 rows on each comparator, all path syntax
rather than comparison: U+0000 in `alone` and `between`, `.` and `/` alone
(each resolves to the directory, `EEXIST`), and `a/b` (`ENOENT`). Every other
disposition, collision, stored unit, catalog position and fold row follows from
the pinned data and these rules, and `fsck_hfs -n`, whose key order check runs
over the same 1.2 million records, finds the volume clean once its two illegal
names are removed. `differences.tsv` on `H+` holds 28 `chain` rows: the prediction
folds the 30 exception units to their lowercase, which breaks the ascending
order of the chain in 28 places; the exceptions remove them, and `HX` has none.

Fold table. The classes on `H+` beyond the `HX` ones come from Unicode 3.2.0's
simple lowercase on pages `0x00`, `0x01`, `0x03`, `0x04`, `0x05`, `0x21` and
`0xFF`, and the Georgian capitals' mapping U+10A0 to U+10C5 onto U+10D0 to
U+10F5. Of the 443 units on those pages with a lowercase, 304 are observed (176
`determined`, 128 `ambiguous`) and 139 are `unobservable`, because they
decompose. The rule is lowercase on those pages unless the unit decomposes, so
each of the 139, from U+00C0 on, folds to itself, which no observation checks.
Among the observed rows
no fold changes a unit on any other page, and no fold occurs that this rule does
not predict. These 30 observed units on the folded pages have a Unicode 3.2.0
lowercase but fold to themselves, and are the rows of
`tools/hfsplus-unicode-exceptions.txt`:

| Units | Their Unicode 3.2.0 lowercase |
|---|---|
| U+01A6, U+01F6, U+01F7 | U+0280, U+0195, U+01BF |
| U+03D8, U+03DA, U+03DC, U+03DE, U+03E0, U+03F4 | U+03D9, U+03DB, U+03DD, U+03DF, U+03E1, U+03B8 |
| U+048A, U+048C, U+048E | The next unit |
| U+04C5, U+04C9, U+04CD | The next unit |
| U+04D4, U+04D8, U+04E0, U+04E8 | The next unit |
| U+0500 to U+050E, even units | The next unit |
| U+2126, U+212A, U+212B | U+03C9, U+006B, U+00E5 |

The same rule needs no exception for 30 more observed units with a lowercase
outside the folded pages: U+021C, U+0220, U+0222, U+0224 and U+24B6 to U+24CF
fold to themselves because their pages are not folded. The Deseret case pairs
are supplementary values, covered by the supplementary claim below. U+212B is
observed by order only: its lowercase U+00E5 decomposes, so no stored name holds
U+00E5, and the row says where U+212B sits on the chain, not that it folds to
itself rather than to U+00E5. U+212B has no decomposition under the rule (U+2000
to U+2FFF are unchanged), so it is an exception row and folds to itself.

Ignorable units. On `H+` the 16 units above collapse: `aXb` collides with `ab`,
and in another directory `aXb` is created and keeps the unit. A name of one
ignorable unit is legal (names of several, or mixed with other units after the
first, were not probed): U+200C alone is created, stores the unit, and takes
rank 0 among its siblings that are not threads, since the leaf walk skips thread
records; the other 15 alone collide with it. So an empty folded key differs from
a thread's empty name.
`fsck_hfs -n` accepts this. On `HX` nothing is ignorable, and all 16 are ordinary
units.

U+0000. A POSIX path cannot carry U+0000, and the system's substitution reaches
it: U+2400 in a POSIX name stores U+0000, and `readdir` returns U+2400 for it, in
the same way `:` stores U+002F and `readdir` returns `:`. These are the only two
values whose `readdir` form differs from the stored one, on both comparators.
`fsck_hfs -n` accepts a stored U+0000 at the start, the middle and the end of a
name: U+2400 stores U+0000 first in `U+2400 a`, in `a U+2400 b` and last in `a
U+2400` (leaf units `0000.0061`, `0061.0000.0062` and `0061.0000`), on volumes
of their own in `e3-small-names/`, with exit 0 on both comparators. On `H+` the name sorts
last among 240588 siblings, after U+FFFD, and `a` U+0000 `b` sorts after `a`
U+FFFD `b`; on `HX` it has rank 0.

Decomposition. For every BMP value the stored units equal canonical
decomposition, recursively, with U+2000 to U+2FFF and U+F900 to U+FAFF unchanged,
the Hangul algorithm applied, and compatibility mappings ignored. 12142 BMP
values therefore never store their own unit. `H+` and `HX` agree on the stored
form of every value. No supplementary value decomposes: the 13 musical symbols
U+1D15E to U+1D164 and U+1D1BB to U+1D1C0, which have canonical decompositions in
Unicode 3.2.0, are stored as their own surrogate pairs. The canonical singletons
give the only `HX` collisions: U+0340, U+0341, U+0343, U+0374, U+037E, U+0387 and
the 20 U+1Fxx values collide with their targets.

Combining order. The `pairs` set holds `a` followed by every ordered pair of the
327 marks of nonzero class (327 squared names, not all marks and not longer
runs). Stored order equals a stable sort by Unicode 3.2.0 class for every pair
of the 297 BMP marks, including the ones that decompose into marks; the
reorderings seen are all of two-mark runs. Three BMP values have class 0 yet
decompose into marks with no starter: U+0F73 (U+0F71 U+0F72), U+0F75 (U+0F71
U+0F74) and U+0F81 (U+0F71 U+0F80). `e3-small-names/class0-VARIANT.tsv` holds,
for each, `a` with the value alone, its decomposition, the decomposition
reversed, and its second mark before and after it. On both comparators each
value stores its decomposition; `a` U+0F72 U+0F73 and `a` U+0F73 U+0F72 collide
and store U+0F71 U+0F72 U+0F72, and the reversed decompositions collide with the
value and store sorted, likewise for the other two. A stable class sort applied
after decomposing predicts all 15 names (`class0-VARIANT-differences.tsv` is
empty), and a sort applied before decomposing, with the class 0 value acting as a
starter, is ruled out: it would keep U+0F72 ahead of U+0F71. The 30
supplementary marks are not
ordered: `a` U+0300 U+1D165 stays in the order given, on both comparators, as
does `a` U+1D165 U+0300, which is why the two do not collide. This accounts for
7421 `units-supplementary` and 7421 `representative-supplementary` rows in
`differences.tsv`, and for nothing else in `pairs`.

Longer runs and precomposed starters. `e3-small-names/marks-VARIANT.tsv` holds
1003 names on one 8 MiB volume per comparator: the `SET.tsv` columns, with
`scope` the category, then `predicted_units`, `predicted_representative` and
`agrees`. The prediction is the design's rule: full canonical decomposition,
then a stable sort by Unicode 3.2.0 class of each maximal run of nonzero class,
a supplementary value as class 0. `agrees` is `yes` for all 1003 names on both
comparators, and the two files are identical: 233 names are created and 770
collide with an earlier ordering, each storing exactly the predicted units, so
the rule has no counter-example in these categories. A name not already in an
earlier category counts once.

| Category | Names | Created | `EEXIST` |
|---|---|---|---|
| `run3`: `a` and every ordered three of U+0334 (class 1), U+0323 (220), U+0300 and U+0301 (230), U+0345 (240) | 125 | 42 | 83 |
| `run4-distinct-classes`: every ordered four of U+0334, U+0323, U+0300, U+0345 | 256 | 35 | 221 |
| `run4-equal-classes`: every ordered four of U+0334, U+0323, U+0300, U+0301 | 175 | 42 | 133 |
| `run3-script-marks`: every ordered three of U+05B0 (10), U+05B8 (18), U+05BC (21), U+0F71 (129), U+0F72 (130) | 125 | 35 | 90 |
| `decomposable-in-run`: U+0344, U+0F73, U+0F75, U+0F81, U+0340, U+0341, U+0343 alone, before, between and after U+0334, U+0323, U+0345 | 175 | 43 | 132 |
| `run-split-by-starter`: marks, a decomposable mark, `b`, then marks again | 10 | 8 | 2 |
| `precomposed-starter`: U+00E2, U+1EAD, U+00C5, U+1E0B, U+AC00, U+AC01 and U+D7A3 with marks after and before, and every order of their decomposed spellings | 24 | 10 | 14 |
| `hebrew`: U+05E9 and every order of U+05C1, U+05BC, U+05B8 (and U+05B0) | 30 | 2 | 28 |
| `tibetan`: U+0F40 and every order of U+0F71, U+0F72, U+0F7C (and U+0F74), and U+0F73 with U+0F7C | 32 | 4 | 28 |
| `long-decomposition`: U+1E69, U+01D5 and U+1FA8, alone and with marks, and their decomposed spellings | 51 | 12 | 39 |

A run is sorted as a whole, not pairwise: every order of a Hebrew or Tibetan
cluster collapses to one name, and marks of equal class keep their order
(U+0F40 U+0F71 U+0F72 U+0F7C and U+0F40 U+0F71 U+0F7C U+0F72 are two names, both
stored as given, since U+0F72 and U+0F7C are class 130). A decomposable mark in
the middle of a run is replaced by its marks, which then sort with the rest:
`a` U+0345 U+0F73 U+0323 stores U+0061 U+0F71 U+0F72 U+0323 U+0345. A class 0
starter ends a run, so marks do not cross it. A mark ahead of a precomposed starter
stays ahead (U+0323 U+00E2 stores U+0323 U+0061 U+0302), and one after sorts into
the starter's decomposition (U+00E2 U+0323 stores `a` U+0323 U+0302; U+1E69 stores
`s` U+0323 U+0307, U+01D5 stores `U` U+0308 U+0304 and U+1FA8 stores U+03A9
U+0313 U+0345). A Hangul syllable decomposes to its jamo and the mark follows
(U+AC00 U+0323 stores U+1100 U+1161 U+0323). `fsck_hfs -n` accepts both volumes
(`marks-fsck.tsv`: `CheckHFS returned 0`), and flags no name.

Hangul. Every one of the 11172 syllables stores its two or three conjoining
jamo, and its explicit jamo spelling collides with it, on both comparators.

Supplementary characters. A value above U+FFFF is stored as its pair. None of
the 177823 probed values collides with another, including the Deseret case
pairs, and catalog order is unsigned unit order: in `between`, all 177823
names occupy one run of ranks, in strictly ascending unit order, that starts
right after `aU+D7FFb` and ends right before `aU+E000b`, on both comparators.
The tool checks this when it derives `fold.tsv`. All 2048 surrogate units are
therefore stored, but only as parts of pairs, and their rows are `pair-only`.
A low unit is compared only with a low unit, in the same position of another
pair; a high unit is compared with another high unit, and with a BMP unit
standing in the same position of another name, which the run between U+D7FF and
U+E000 bounds. What is observed is the order within each group: adjacent lows
occur under one high, and adjacent highs have overlapping sets of lows, so no
two units of a group compare equal. No name holds a surrogate unit alone, so no
value of its fold is observed.

Refused spellings. U+FFFE and U+FFFF are valid scalar values, yet the system
stores each as the ASCII escape of its UTF-8, such as `%EF%BF%BF`, and `readdir`
returns that text. The 2048 three-byte encodings of surrogates are treated the
same way. U+FDD0 to U+FDEF are stored unchanged.

`fsck_hfs` and two mark pairs. The driver stores U+0306 U+0307 and U+0308 U+030D
as given, on both comparators, and `fsck_hfs -n` calls a name holding either
sequence an `Illegal name`, exiting 8. Under `-d` it proposes U+0310 and U+0308
U+0301 as their replacements. Among all the ordered pairs of the 327 marks
after `a` these two alone are flagged. `e3-small-names/` (`fsck.tsv`,
`commands.txt`) holds 25 names per comparator, each alone on an 8 MiB volume.
Each sequence, in 16 spellings in all, makes `fsck_hfs -n` exit 8 with an
`Illegal name` after `a`, `A`, `b` and `e`, with nothing before the marks, in the
middle of a name (`a b`, the marks, `c`), and after the precomposed U+0103 and U+00E4
(stored `a` U+0306 U+0306 U+0307 and `a` U+0308 U+0306 U+0307 for the first
sequence). It exits 0 on `a` U+0307 U+0306, `a` U+030D U+0308 and `a` U+0308
U+0301 U+030D, so the check is on the adjacent sequence. A stored U+0000 (three positions), `U+200C` alone (ignorable on `H+`,
an ordinary unit on `HX`) and `a:b`, which stores U+002F, each exit 0. The
volume holding such a name fails this `fsck_hfs` (version hfs-715.160.9),
wherever the name came from.

The 21 base sequences of the repair routine (see the cross-check) were then run
the same way (`fsck-sequences.tsv`, 32 names per comparator). On `H+` and on `HX`
alike, with identical rows, each of these makes `fsck_hfs -d -n` exit 8 and print
an illegal name, alone in a name and, for the six that do not use a Greek
letter, after `a`: U+00A8 U+030D (replacement U+00A8 U+0301); U+030D after each
of U+0391, U+0395, U+0397, U+0399, U+039F, U+03A5, U+03A9, U+03B1, U+03B5,
U+03B7, U+03B9, U+03BF, U+03C5, U+03C9 and U+03D2 (the letter and U+0301);
U+09AC U+09BC (U+09B0); U+0A21 U+0A3C (U+0A5C); U+0B2F U+0B3C (U+0B5F);
U+0E4D U+0E32 (U+0E33); and U+0ECD U+0EB2 (U+0EB3). The driver stores every one as
given, in the order given. U+0FB2 or U+0FB3 followed by U+0F80 U+0F71 is stored
as the first, U+0F71, U+0F80 (class 129 before class 130), and `fsck_hfs` exits
0 on it. Three controls exit 0 too: U+0391 U+0301, U+0E32 U+0E4D and U+09AC
alone. No sequence was stored differently from its input except the two
Tibetan ones.

What cannot be observed. A fold row is visible only through a stored unit. Of
the 14194 units that no POSIX name stores alone, 12146 appear in no stored name:
the 12142 that decompose, U+FFFE and U+FFFF, and U+003A and U+2400, which the
system maps away. The other 2048 are the surrogate units, which appear only as
parts of pairs (`pair-only`): their order within each group is observed, never a
value. A catalog holding one of the 12146 is outside what this driver produces
on macOS, and for POSIX names none of those rows can be compared; no evidence
here covers a catalog written by another implementation. U+0000 sorts
after every unit up to U+FFFD, and its relation to U+FFFE and U+FFFF is not
observed: the table gives U+0000 the fold 0xFFFF, as does NetBSD's table, so a
stored U+0000 and a stored U+FFFF compare equal. No POSIX name stores U+FFFF. The pairs cover two marks after one starter (plus the three class 0
values above) and the marks set covers the longer runs and precomposed starters
it lists; runs of five or more marks, names of several ignorable units,
unassigned supplementary noncharacters and other malformed UTF-8 are not probed. The `HX` answers cover
compare type `0xBC` only: `specs/design-hfsplus.md` also allows `0xCF` on `HX`
and 0 on `H+`, and `newfs_hfs` has no option that makes either (`-s` selects
`0xBC`), so they were not probed.

Generated tables. `tools/gen-hfsplus-tables.rb` generates
`lib/formats/hfsplus/unicode-tables.h` from `UnicodeData-3.2.0.txt` and the
exceptions file. `tools/hfsplus/check-unicode-tables.rb` is the gate: it parses
the header, shares nothing with the generator, verifies the digests of its
inputs (`e3-hfsplus/`, `e3-hfsx/`, `e3-small-names/`, the Unicode file and NetBSD's
file), exits nonzero on any violation, and is run by hand, because its inputs live
outside the tree. `HX` uses the identity, since compare type `0xBC` is binary. It
finds no violation:

* Fold rows, on each comparator by the three rules above: every `determined` row,
  every chain position with one fold that is listed and strictly above the one
  before, the 16 `ignorable` rows, U+0000's rule, and the 2048 `pair-only` rows in
  strictly ascending unit order, with U+D7FF below U+D800 and U+DBFF below U+E000.
  The chain has 51013 positions on `H+` and 51341 on `HX`. The checker's self-test
  alters two tables that each pass a row-by-row membership test: U+0410 alone
  folding to itself breaks one fold per chain position, and U+0411 and U+0431
  folding to U+0411 breaks the ascending order. Both are rejected, as is a copy of
  the header with U+0410 altered, with exit 1.
* Stored units, on each comparator: for every row of `alone`, `between`, `pairs`,
  `hangul` and `surrogates`, and of `e3-small-names/class0-VARIANT.tsv` and
  `marks-VARIANT.tsv` (its self-test gives U+0323 class 230 and requires the
  rows to reject it), table
  decomposition and the stable class sort reproduce `leaf_units`, with `:`, U+2400,
  U+FFFE, U+FFFF and the 2048 surrogate encodings spelled as this section explains
  and supplementary values as their pairs. The rows with no leaf are the 5 of
  `residual.tsv`.
* Unicode data: every BMP combining class and every decomposition of the header
  equals `UnicodeData-3.2.0.txt`'s (canonical, recursive, none in U+2000 to U+2FFF
  or U+F900 to U+FAFF, Hangul by formula), read by the checker itself; 65536
  classes and 970 decompositions compared. The checker also asserts the three
  digests below, so a table change fails until they and this record are updated
  deliberately, after review. Its self-test gives U+0031 class 230, and U+00C0 a
  wrong decomposition, and requires the comparison alone to reject both.
* Collisions: each colliding name has the catalog key of its representative.
* Catalog order: sorting each directory's names by the comparator's key gives
  exactly `rank`, and no two siblings compare equal.

| Set | Names sorted | Directories on `H+` | Directories on `HX` | Collisions on `H+` | Collisions on `HX` |
|---|---|---|---|---|---|
| `alone` | 241308 | 16 | 2 | 720 | 26 |
| `between` | 241310 | 17 | 2 | 721 | 26 |
| `pairs` | 106929 | 4 | 4 | 35423 | 35423 |
| `hangul` | 22344 | 2 | 2 | 11172 | 11172 |
| `surrogates` | 2048 | 1 | 1 | 0 | 0 |
| `class0` | 15 | 3 | 3 | 9 | 9 |
| `marks` | 1003 | 24 | 24 | 770 | 770 |

The table encodes U+0000 as the fold 0xFFFF on `H+`, the largest value, because
a fold of zero means ignorable; the binary comparison compares it as itself, so it
sorts first. U+FFFF also folds to 0xFFFF, so the two compare equal, as in
NetBSD's table; no POSIX name stores U+FFFF, and their relation is not observed.

SHA-256 of the tables' serializations, printed by the checker: the fold table as
65536 big-endian units, the combining classes as 65536 bytes, and for each
decomposing unit in order the unit, its count and its units, as big-endian units.

```
b8e22686df0e37d75ab4e0d24c83cf92a5cab407cb6ff722fa54384d6544be46  fold
0c753eab520b1374925dc1128228dfe56987f5284ee78837e116a89190ebc599  combining classes
654f209716fc62ecf5d15fff1063cc021d177bf2c0e97918b60ee93569c70496  decompositions
```

NetBSD witness. `~/hfsplus-captures/netbsd-libhfs/libhfs.c`, from
<https://raw.githubusercontent.com/NetBSD/src/e4f73ac629cd72caf75fdbe9eac3eeaafeaca1f0/sys/fs/hfs/libhfs.c>
(revision 1.19), SHA-256
`d4aeef80a6b06232340182da3d74bc91e03bfc39de084713eac2d714f10d7f94`. The checker
interprets the statements of `hfslib_create_casefolding_table` and compares the
resulting folds over all 65536 units. The file has no decomposition or combining
class data. Taken alone, NetBSD's table satisfies every rule of `fold.tsv`.

The generated table equals it on all 65536 units, and any difference fails the
checker. On the 53390 observable rows the agreement confirms the 30 exceptions,
the Georgian mapping, the 16 ignorable units, the lowercase choice in the 124
ambiguous classes and the fold of U+0000, without NetBSD having seen this
observation. On the 12146 unobservable rows it is the rule itself: NetBSD
leaves the 139 units on the folded pages that have a lowercase and decompose as
themselves, and the table lowercases on those pages unless the unit decomposes.
No POSIX name stores one of those units, so no observation here chooses between
two folds for them; a catalog written by another implementation that holds one
undecomposed folds it as NetBSD does.

### E4 Hard links

Status: answered, with one open item (hard-linked symbolic links). The
directory hard-link stub test has its native bytes, the stub and indirect
node of `h-hfsplus-16777216-e4-dirlinks-J` below; the same test on a
hand-patched record is not run.

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

The stub test needs type `hlnk` and creator `hfs+`. Observed accepted:
`createDate` equal to the private data folder's or the root's. Observed
refused: the file's own date. Not tried: the volume header's date, and a
`special` that names no node. The evidence is hand-typed mounts of patched
copies, not a capture. After the root's `createDate` was set
earlier (`SetFile -d`), links made earlier still read as links and a new stub
took the private folder's date, so the dates the test accepts include the
private folder's and the date given to new stubs is only the private
folder's. A Finder alias (`fdrp`/`MACS` with Finder flag `0x8000`, and
`alis`/`MACS`) set through `xattr` is an ordinary file, as is every file whose
Finder information carries `hlnk`/`hfs+` with another date. The `fdrp`/`MACS`
rows cannot show the directory-link test: the target `dir_20` does not exist,
so a passing test and a failed lookup cannot be told apart.

Refusals. On a volume that is not journaled `link(2)` on a directory fails with
`EPERM` from every place tried (`h-hfsplus-16777216-e4-dirlinks-plain`, seven
attempts). `link(2)` on a symbolic link follows it and links the target (`sl/follow` and
`sl/target` share an inode with `nlink` 2); `ln -P`, which links the symbolic
link itself, fails with `Operation not supported`. A C `linkat(AT_FDCWD, src,
AT_FDCWD, dst, 0)` on a symbolic link, on a journaled and on an unjournaled
8 MiB volume, returns -1 with `errno` 45 (`ENOTSUP`) and creates no name;
`AT_SYMLINK_FOLLOW` succeeds and links the target (inode and `nlink` 2 of the
target file). macOS does not create a hard-linked symbolic link through these
calls.

Directory links (`h-hfsplus-16777216-e4-dirlinks-J`, `newfs_hfs -J`, mounted
`journaled`; `capture.rb --populate e4-dirlinks`). `link(2)` on a directory
succeeds on a journaled volume in four of the seven places tried: a populated
`dl1/a/d` to `dl1/b/d` (different parents); an empty `dl3/a/x/e` to `dl3/a/e2`
(the destination's parent is the source's grandparent); an empty `dl4/a/x/e` to
`dl4/c/e3`; and a third name for a directory that had two (`dl7/c/d`, after
`dl7/a/d` to `dl7/b/d`). It fails with `EPERM` for the same parent
(`dl2/a/d` to `dl2/a/d2`), for a destination directly in the root (`dl5`) and for a
source that is a top-level folder (`dl6-top`). Both names then `lstat` as one
directory (inode 22, `nlink` 3, children listed through either). The hypothesis
that the destination's parent may not be an ancestor of the source is wrong:
`dl3` succeeded with it. The conditions tried are not the whole rule.

The records, from `fields.json` of that capture. The first link (`dl1`) made
two stubs, IDs 25 (in `dl1/a`, folder 21) and 26 (in `dl1/b`, folder 23), both named
`d`, and the indirect node ID 22:

| Field | Stub (file record) | Indirect node (folder record `dir_22`) |
| --- | --- | --- |
| Place | in the folder of the name | folder 19, `.HFS+ Private Directory Data` + CR (flags `0x0010`, mode `041555`, owner flags 2, valence = the number of nodes) |
| `flags` | `0x0022` (thread and the link-chain bit; no date-added bit) | `0x00A4` |
| Codes | user information `66647270 4d414353 8000 0000 00000000`: type `fdrp`, creator `MACS`, Finder flags `0x8000` | none (user information zero) |
| `special` | 22, the indirect node's ID | 2 (3 in `dl7`): the number of names |
| owner, group | the chain: stub 25 has owner 26 and group 0; stub 26 has owner 0 and group 25 | mounting user 501, group 20 |
| mode, owner flags | `0100444`, 2 | `040755`, 0 |
| dates | all four equal the private folder's creation date | all four the creation time |
| Finder information | zero (no date-added word) | `000000006aeaeb9f 00000000 00000002` for `dir_22` (the date added and the write counter), counter 0 for the empty `dir_34` |
| forks | empty | not applicable; `valence` 1 (the populated `d`) or 0 (empty directory) |

Against a Finder alias: the look-alike made with `xattr` (`code-fdrp-MACS-alias`
of the E4 capture) has the same user-information bytes (`fdrp`, `MACS`, Finder
flags `0x8000`), so a directory link's codes are those of a Finder folder alias.
The records differ in `special` (1 against 22), `flags` (`0x0082` against
`0x0022`), mode (`0100644` against `0100444`), owner flags (0 against 2), owner and
group (501 and 20 against the chain) and the Finder information (a date added
against zero). The alias used here is that look-alike, not one made by the
Finder; no alias made by the Finder is recorded. The directory-link stub test of
a hand-patched record was not run: `patch_record.rb` does not set the Finder
flags, owner flags, mode or owner and group, and a patched file would not be
backed by an indirect node of the same pattern without them.

Evidence rows. Answered: stub and indirect-node fields; the stub test (codes,
dates, flag) for files; whose metadata macOS reports; link counts; chain
fields; macOS's own attributes on indirect nodes (none); the Finder alias and
`hlnk`/`hfs+` look-alikes read as ordinary files; the native directory-link stub
and indirect node on a journaled volume (above); that macOS creates no
hard-linked symbolic link through `link(2)`, `linkat(2)` or `ln -P`. Open: the
hard-linked symbolic link's native record, because none can be made; a reader
meets one only on a volume another system wrote. Not run: a directory-link
stub test with a patched record against a missing target; the native stub
above supplies the bytes the detection needs.

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
| Inline limit | values of 0, 1, 2, 3, 5, 7, 17 bytes and up to 3802 bytes (measured at a 16-unit name, 8192-byte nodes) are `0x10` inline records; 3803 bytes and above are `0x20` fork records. Fork records of 3803 to 4000 bytes take one block, 5000 and 8000 bytes two, 70000 bytes 18 (one extent, `750+18`). No `0x30` record appears: the forks hold at most one extent |
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
bytes, measured with the 16-unit name `com.fsimg.sz3802` in an attributes tree
of 8192-byte nodes; whether the limit varies with the name length or the node
size is not established.

### E6 Compression

Status: answered.

Procedure. `capture.rb --populate e6-TOOL` for TOOL `zlib`, `lzvn` and `lzfse`
(`afsctool -c -T CODEC -s 0 -v`) and `ditto` (`ditto --hfsCompression`), each on
HFS Plus (`h-hfsplus-8388608-e6-TOOL`) and `lzfse` also on HFSX
(`h-hfsx-8388608-e6-lzfse`); `e6-sweep` on `h-hfsplus-100663296-e6-sweep` (the
capture keeps fields only). Content is deterministic text, random bytes
or zeros of 0, 1, 65535, 65536, 65537 and 200000 bytes, text of 131073, 262145,
393217 and 589825 bytes, and a 196608-byte file whose second 65536-byte chunk is
random (`mixed`); `afsctool` also runs with `-L` (`forced-*`). Every manifest
records SHA-256 of the file read through the kernel, which decompresses it, so
the digest is of the decompressed bytes. `fields.rb` reads the
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
11, 7: attribute sizes of 100, 514, 200 and 514 bytes, payloads of 84, 498, 184
and 498) and 65537 bytes in the resource fork, at 1, 4, 16 MiB too; no inline
file is larger than 65536 bytes. Sizes below are whole attribute sizes, the
16-byte decmpfs header plus the payload. `afsctool` stores text inline up to
the largest attribute observed, 3612 bytes for zlib (payload 3596, from 12000
bytes of text), 3247 for LZFSE (3231, from 10000), 3293 for LZVN (3277, from
8000), and moves the next size tried to the resource fork (16000 bytes of text
zlib, 12000 LZFSE, 10000 LZVN). All three are below the 3802 bytes of the
largest inline attribute measured in E5, but the sizes tried are 2000 bytes
apart and the attribute that would have followed is not observed, so the data
neither show that the compression limit equals 3802 nor contradict it.

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
(`[16, 25811, 51642, 51644]` for 3 chunks, 44 for 10). For one chunk the two
rules coincide, a first offset of 8. Chunk `i` spans
`[offset[i], offset[i + 1])`. LZVN chunks begin with the LZVN opcode bytes
(`e0`, `ef`) and end with `06`; LZFSE chunks begin `bvx2`, the last short chunk
`bvx-`.

Evidence rows. Answered: header, types written per tool, raw markers (zlib
`ff`, LZVN `06`, LZFSE none), type 4, 8 and 12 framings, largest inline size
(65536 uncompressed; the largest attribute observed is 3612 bytes, whole attribute size, for zlib), data
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
prints `Volume is journaled.  No checking performed.` on every journaled volume,
so these captures have no native verdict: their `fsck_populated.passed` is true
for that message although nothing was checked. The decoder reports the journal
fields and verifies no checksum.

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
| Backing image copied while writing (a copy torn during writes) | differ in 10 of 20 trials: none of 8 at 0.1 and 0.3 seconds (still 512, 512), 2 of 4 at 0.6, 4 of 4 at 1.5 and 3.0 | `0x80002000` | Stale (below) |
| The same image mounted again | equal after all 40 remounts (`start` = `end` = 510976 in `e8-remounted`); 10 of the 40 began with transactions pending, the other 30 began clean | `0x80002100` | Replay, seen for those 10 only, through a macOS mount |

Rule that separates the states. The journal is a circular log whose header
`start` and `end` are byte offsets into the journal, each at least `jhdr_size`
(512) and below `size`. `start` = `end` means no transaction is pending, at any
position: the clean volumes above sit at 63488, 113664 (the NPS `hfsjtest1`
specimens), 159744, 316928 and so on. `start` is not equal to `end` when
transactions are pending (this rule is observed on these images only), and `start` may be greater than `end` once the log has
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
procedure produced `start` not equal to `end` natively. A copy taken while the
writer runs shows what a copy torn during writes looks like: it can be torn
between the journal header, the journal body and the volume header, so it is not
evidence of what a crash leaves. Replay was seen only through a macOS mount.
The copy procedure's yield depends on timing (0 of 8 under 0.6 seconds, 10 of 12 at
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
volume OK. Creating 3000 empty files took 0.52 seconds in a probe (Throwaway probes), so
the files are made with plain `File.write` calls. The images and manifests are kept gzipped (`image.img.gz`,
`manifest.json.gz`, about 6 MiB per capture); `fields-index.json` beside each
`fields.json` is `fields.rb --json --index --records 5` of the image.

| Stage | Files | Catalog leaf records | Leaf nodes | Depth | Root records | Level 2 index nodes (records each) |
| --- | --- | --- | --- | --- | --- | --- |
| `e9-create` | 100006 | 200120 | 8203 | 3 | 65 | 65 (61 to 132) |
| `e9-delete` | 64659 | 129426 | 7960 | 3 | 64 | 64 (61 to 132) |
| `e9-thin` | 1271 | 2650 | 2283 | 3 | 64 | 64 (13 to 50) |
| `e9-sparse` | 13 | 134 | 56 | 3 | 52 | 52 (1 to 2) |

Catalog records are a file or folder record and its thread record each (200120 =
2 * (100006 + 54)). The file counts do not follow from the deletions alone:
`e9-delete` deleted 35350 files, and 100006 - 35350 = 64656, but its volume
header says 64659. The two manifests list the visible trees: `e9-delete` has
two `.fseventsd` log files that `e9-create` lacks (`00000000c752d940`,
`00000000c752d941`), which accounts for two of the three, as macOS wrote them
between the mounts. The third file is in neither manifest, and its origin is
not established. The catalog fork is 314572800 bytes in one extent
(`15366+76800`), 76800 nodes, and `fork_size_matches_nodes` is true.

Map nodes. The catalog's header node holds 3840 map bytes (`4096 - 256`, 30720
bits), so 76800 nodes need map nodes: the header's `fLink` is node 2, and nodes 2
and 3 are map nodes (kind 2, height 0, one record, `bLink` 0 on both, node 2's
`fLink` 3, node 3's `fLink` 0). Each map record is 4076 bytes (`4096 - 20`) at
offset 14, so its record offsets are `[14, 4090]` and two bytes lie between the end
of the record and the 4-byte offset table at 4092. The three maps cover 95936 bits
(`30720 + 2 * 32608`); bits at and beyond 76800 are zero; the map nodes themselves
are marked in use (the formatted volume has 4 bits set and the first clear bit is 4:
nodes 0 to 3). The record length `M - 20` was
observed at node size `M` = 4096 only: `-n e=1024 -I e=1700` and `-n e=2048 -I e=3400`
leave the extents tree at 512 and 256 nodes, and an 8192-byte catalog tree needs
more than 63488 nodes (about 500 MiB) to have map nodes, which was not tried.

Index keys. Every index record in the 249 index nodes of the four stages was
compared with the first key of its child (8203, 7960, 2283 and 56 leaf children
at level 2 and the level 2 nodes under each root): an index record's key equals the
first key of the child it names in every stage (`key_mismatches` 0) and the child's
height is one less. After the deletions this is not trivial: 2343 of the 7960 level-2
keys of `e9-delete` are not keys of `e9-create`. From these set differences it is
inferred, not observed directly, that macOS rewrites an index key when its
child's first record is deleted: no capture holds an index key before and after a
single deletion.

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

Status: partly answered. Four HFS Plus volumes from 2001 to 2003 are decoded and
scanned: three Mac OS X install discs and the Mac OS 9-formatted volume of
`mozilla-1.0-smi`. No HFS Plus volume written by Mac OS 8.1 to 9.2 with
non-ASCII names was found: seven Mac OS 8.1 to 9.2.2 install discs and disk
images from the Internet Archive are classic HFS and were discarded. What
stays open is the Unicode 2.1 spelling question (below).

Procedure. The owner approved Internet Archive images of the Mac OS 8.1 to 9 era
for local testing only: they stay outside the repository, are never
redistributed, and are cited by item identifier. Items were chosen from the
archive.org advanced-search and metadata APIs (file lists and sizes), fetched one
at a time with `curl -C - --retry 3 --fail -L` under a generic client name, and
never attached, mounted or executed. A `7z` holds a MODE1/2352 CD image; the
user data (16 bytes into each 2352-byte sector) is copied out, the Apple
Partition Map read, and the `Apple_HFS` partition copied with `dd`. The disc
partitions start at 512-byte sector 928 or 968 and hold a wrapper (E11). `fields.rb`
decodes each partition with no change; a throwaway scanner (not committed) walks
the catalog leaf chain. The corpus records the specimens in `SOURCES.md`,
`MATRIX.md` and `manifest.sha256`.

| Specimen (archive.org item) | Volume | `lastMountedVersion` (primary, alternate) | Blocks | Catalog | Leaf records |
| --- | --- | --- | --- | --- | ---: |
| `archive-macosx-10.0-1z691-2974-a` (`1Z691-2974-A`, Mac OS X 10.0 CD) | wrapped; outer 73728-byte blocks, embedded 8029 of them | `8.10`, `8.10` | 4096 x 144522 | 4096-byte nodes, depth 3 | 11176 |
| `archive-macosx-10.1-2z691-3184-a` (`2Z691-3184-A`, 10.1 upgrade CD) | wrapped; 9102 outer blocks | `8.10`, `10.0` | 4096 x 163836 | 4096-byte nodes, depth 3 | 12564 |
| `archive-macosx-10.3-disc2-2z691-4586-a` (`2Z691-4586-A`, Panther disc 2) | wrapped; 9189 outer blocks | `10.0`, `10.0` | 4096 x 165402 | 4096-byte nodes, depth 3 | 3464 |
| `mozilla-1.0-smi` (Mozilla 1.0 for Mac OS X, 2002) | bare `H+` at byte 0, no partition map | `8.10`, none | 512 x 67900 | 4096-byte nodes, depth 2 | 876 |

`mozilla-1.0-smi` decodes statically: MacBinary II around an NDIF self-mounting
image whose `bcem` 128 resource lists 134 chunks (130 ADC-compressed, 2 zero, 1
ignored, 1 end). Decoding the ADC chunks (a literal run, a two-byte and a
three-byte back reference) gives 67900 sectors, and the volume header sits at
byte 1024. The NDIF marks the last two sectors as ignored: the alternate header
(and the reserved sector after it) is not in the image, so the alternate reads as
zeros and `fields.rb` reports it in `problems`; that comes from the container, not
the decoder.

Volume headers. All four have version 4, `attributes` `0x00000100` (only the
cleanly-unmounted bit 8), `journalInfoBlock` 0, no journal, no attributes tree
(zero `attributesFile`), `lastMountedVersion` `8.10` or `10.0` (never `FSK!`), a
zero boot area and a bitmap whose clear bits equal `freeBlocks`. The Mac OS X discs hold a 1 MiB startup file (256
blocks); the smi volume has none. The 10.0 and 10.1 volumes keep nonzero Finder
information words 0, 2 and 5; the smi volume's eight words are zero.
The wrapped discs carry `drAtrb` `0x8300` on the outer volume (as in E11) and the
embedded volume is exactly `count * drAlBlkSiz` bytes, so `N * B = V`.

Trees. Extents tree: 1024-byte nodes (265 on the smi volume, 3072 on the 10.0 disc), `maxKeyLength` 10,
attributes `0x00000002` (big keys), `keyCompareType` 0, `btreeType` 0, no
leaf records. Catalog tree: 4096-byte nodes on all four (including the 512-byte-block
smi volume), `maxKeyLength` 516, attributes `0x00000006` (big keys and variable
index keys), `keyCompareType` 0, `btreeType` 0; the index keys equal their
children's first keys (`key_mismatches` 0). Every value lies inside the tree
parameter table of the design.

Catalog flags. Folder records: flags `0x0000` only (no folder-count-valid bit).
File records: `0x0002` only. No bit outside the closed tables of the design
appears, and no volume attribute bit other than 8.

BSD information. The Mac OS 9-formatted smi volume has all 16 bytes zero in every
folder and file record (438 records), text encoding 0 and backup date 0. The
Mac OS X volumes have it set in every record: owner 0 with group 0, 80 or 99, modes
`0100444`, `0100644`, `0100664`, `040555`, `040755`, `040775`, `0120777`,
`0100555`, `0100755`, `040777` and so on, with `adminFlags` and `ownerFlags` 0 in
the commonest forms (10.0: owner 0 or 99, group 0 or 99; 10.1: group 0 or 80;
10.3: group 0 or 80, and the private data folder with mode `040000` and
`adminFlags` 2). Text encodings other than 0
occur on the 10.0 disc (14 records, encoding 1) and the 10.1 disc (encodings 1, 2,
3 and 25); the design ignores them. On the 10.0 disc 5297 backup dates read
2082873600, which is 1970-01-01 08:00:00 UTC; the design does not check backup
dates.

Hard links. The 10.0 and 10.1 discs hold the pre-10.2 hard-link layout: 18 and 20
stub records (type `hlnk`, creator `hfs+`) whose `fileMode` is 0 and whose
`special` is a reference such as 123022516, resolved to 8 and 9 `iNode<ref>` files
inside the private data folder (CNID 16) whose `special` holds the link count (2
and 3). The stubs' `createDate` equals the root folder's on both discs, and the
private data folder's as well on the 10.1 disc (the folder was created at
volume-creation time there); the 10.0 disc's private folder date differs.

Names. The private data folder (four U+0000, then `HFS+ Private Data`) is present
on each Mac OS X disc and ascends in key order, after the other root children,
under the tables' U+0000 fold (0xFFFF, as in E3). The 22 non-ASCII names on the
10.0 disc and the 23 on the 10.1 disc (across both discs, `Espan` + U+0303 + `ol`,
`Franc` + U+0327 + `ais`, `Portugue` + U+0302 + `s`, Hiragino names containing U+3099, a
Japanese name, and `Korean - ` with U+1112 U+1161 U+11AB U+1100 U+1173 U+11AF) were run
through the generated tables of [E3](#e3-unicode)
(`lib/formats/hfsplus/unicode-tables.h`): decomposing each stored name with the
table and stably sorting each run of nonzero classes gives the stored name
itself, for 22 of 22 and 23 of 23, and the leaf keys, compared as the parent ID
and then the folded units with ignorable units skipped, ascend strictly on all
four volumes (0 inversions, 0 equal neighbours over 11176, 12564, 3464 and 876
records). The 10.3 disc and the smi volume have no non-ASCII name (0 of 3464 and
0 of 876 records), so for them only the order is checked.

Open. A Mac OS 8.1 to 9.2 writer's non-ASCII names, which the design says may
follow Unicode 2.1, are unobserved: the only Mac OS 9-formatted volume has ASCII
names, and the Mac OS X 10.0 and 10.1 volumes (made by Mac OS X, probably with
its own tables) show no difference. The tried Mac OS 8.1 to 9.2.2 discs and disk
images are classic HFS (volume names and `drAlBlkSiz`: "Mac OS 8.1" 6656, "Macintosh
HD" 33280, "Mac OS 8.5" 9216, "Mac OS HD" 8192, "Mac OS 9" 10752, "iMac Install CD"
10752, "MACHD" 32256), so a Mac OS 8.1 to 9 volume with an HFS Plus catalog written by
that era's system and with non-ASCII names needs a different specimen.

Evidence row (names and BSD information on Mac OS 9 and early Mac OS X volumes):
answered for BSD information (zero on the Mac OS 9-formatted volume, set on Mac OS X
volumes, `fileMode` 0 on pre-10.2 hard-link stubs) and for the tree, flag and
attribute tables; open for Unicode 2.1 name spellings.

Discrepancies with `specs/design-hfsplus.md` (the designs are not edited here):

* Lines 201 to 205 (alternate header): verification compares the five fork-data
  records plus "the other fields native volumes keep equal after use". The 10.1
  disc's alternate differs from the primary in `lastMountedVersion` (`10.0`
  against `8.10`), `writeCount` (12 against 20) and `modifyDate`; the 10.3
  disc's alternate differs in `lastMountedVersion`, `fileCount`, `folderCount`,
  `freeBlocks`, `nextAllocation`, `nextCatalogID`, `writeCount`, Finder
  information and `modifyDate`, with the fork records equal. A genuine old disc
  then fails a verification that includes those fields. The smi image has no
  alternate header at all (container artifact).
* Lines 495 and 499 (kinds table) against 727 (stub test): the pre-10.2 stubs have
  `fileMode` 0, which the table maps to "Regular file; owner and mode absent"
  and does not list as a stub, while only `0100000` lists the stub. The stub test
  (type, creator and date) must run before the kind lookup.
* Line 728 to 729 (stub date): the 10.0 disc's stubs match the root folder's
  `createDate` and not the private folder's, so the "or" is required.
* Lines 913 to 935 (wrapper): the outer block size is 73728 against an embedded
  4096; the range rules hold (`N * B = V`).
* Line 153 (`lastMountedVersion`): `8.10` and `10.0` are plain values; the
  design's "any value" holds.

No closed-table entry (volume attribute, catalog flag, tree parameter) refuses
these four volumes. The name refusals of lines 555 to 561 do not fire (no `:`,
`.`, `..`, empty or over-long names; the NUL name is the private data folder).

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

Outer tail. The outer fields do not give the image's length. Every captured
wrapper has its alternate master directory block at `image_size - 1024`, and the
image ends past the last outer allocation block by 4096 bytes for the 4096-byte
`mkfs.hfsplus -w` images (16 and 64 MiB, plain and `-J`), 1024 for `-w -b512` and
2048 for Blender 2.28.

Bad block file. The outer extents file is a one-leaf HFS B-tree of 512-byte nodes
whose only record is the key (file ID 5, data fork, start block 0) with the extent
`10+2360`: the bad block file covers exactly the embedded range, and the outer volume
bitmap sets every block. `drAtrb` `0x8300` has bits 15, 9 and 8 set. The outer
volume holds five files (`drNmFls`) that the decoder does not list.

Journaled wrapper. One `mkfs.hfsplus -w -J` image of 16 MiB
(`h-hfsplus-16777216-e11-mkfs-w-J`) has the journal info block's offset (12288)
relative to the embedded volume, with flags `0x5` and no header. After one macOS
mount (it does mount the wrapped volume, writable and journaled) the offset
reads 57344 = 45056 + 12288 and the journal header (`78 4C 4E 4A`) is at image
byte 57344: after the mount the offset counts from the start of the image, not
from the embedded volume. The Apple source cross-check explains it: the mount's
late path adds the embedded start to the stored offset and writes the sum back,
where the early path counts from the embedded volume. It is one observation of
one mount; a second mount of that image was not tried. `fields.rb` reports
`offset_counts_from: image` when the header is found only that way.

Evidence row answered: wrapper layout. The macOS side of a wrapper cannot be made
natively here (no tool formats one), so wrapped volumes come from `mkfs.hfsplus -w`
and the one archival specimen, which agree in every field above except the
geometry.


## Apple Partition Map specimens

The [APM profile](../specs/design-layers.md#apm) rests on these images, read
through fsimg alone and never attached, mounted or executed. The specimens stay
in the local corpus and are recorded here by name. The Mac OS X discs are cooked
from their MODE1/2352 images to 2048-byte sectors outside the corpus. `Entries`
counts the map's entries, with the used ones `i` lists in brackets. `Selected` is
what `i` chooses without `-F`. `t` counts listed members.

| Specimen | Outer layers | `sbBlkSize` | Block | Entries | Selected | `t` | `W` |
| --- | --- | ---: | ---: | ---: | --- | ---: | --- |
| `firefox-1.5.0.1` | UDZO | 2048 | 512 | 2 (2) | 2, `Apple_HFS` | 495 | passes |
| `firefox-1.0` | gzip, UDRO | 512 | 512 | 3 (2) | 2, `Apple_HFS` | 576 | passes |
| `firefox-2.0.0.20` | UDZO | 2048 | 512 | 2 (2) | 2, `Apple_HFS` | 471 | passes |
| `firefox-3.6.28` | UDBZ | 2048 | 512 | 2 (2) | 2, `Apple_HFS` | 328 | passes |
| `firefox-11.0` | UDBZ | 2048 | 512 | 2 (2) | 2, `Apple_HFS` | 140 | passes |
| `firefox-24.0` | UDBZ | 2048 | 512 | 2 (2) | 2, `Apple_HFS` | 156 | passes |
| `firefox-55.0` | UDBZ | 2048 | 512 | 2 (2) | 2, `Apple_HFS` | 159 | passes |
| `firefox-70.0` | UDBZ | 512 | 512 | 4 (3) | 3, `Apple_HFSX` | 169 | passes |
| `firefox-130.0` | UDBZ | 512 | 512 | 4 (3) | 3, `Apple_HFSX` | 166 | passes |
| `mozilla-1.3` | gzip, UDRO | 512 | 512 | 3 (2) | 2, `Apple_HFS` | 760 | fails: alternate header absent |
| `mozilla-1.7.13` | UDZO | 512 | 512 | 3 (2) | 2, `Apple_HFS` | 985 | fails: alternate header absent |
| `python-2.4.4` | UDZO | 512 | 512 | 3 (2) | 2, `Apple_HFS` | 73 | passes |
| `python-3.3.5` | UDZO | 512 | 512 | 3 (2) | 2, `Apple_HFS` | 67 | passes |
| `vlc-1.1.11` | UDBZ | 512 | 512 | 3 (2) | 2, `Apple_HFS` | 1316 | passes |
| `blender-2.28` | UDZO | 512 | 512 | 3 (2) | 2, `Apple_HFS` | 44 | passes |
| `blender-2.33a` | UDZO | 512 | 512 | 3 (2) | 2, `Apple_HFS` | 130 | passes |
| `archive-macosx-10.0` | none | 2048 | 512 | 10 (7) | 9, `Apple_HFS` | 5578 | passes |
| `archive-macosx-10.1` | none | 2048 | 512 | 10 (7) | 9, `Apple_HFS` | 6271 | passes |
| `archive-macosx-10.3-disc2` | none | 2048 | 512 | 10 (7) | 9, `Apple_HFS` | 1730 | passes |
| `archive-darwin-1.0-toast` | none | 2048 | 512 | 10 (7) | none: classic HFS | 0 | fails: no filesystem |
| `macos-7.5-install-cd` | DMG | 512 | 512 | 2 (2) | none: classic HFS | 0 | fails: no filesystem |
| `macbackup1` | none | 512 | 512 | 2 (2) | ISO 9660, bare, with a note | 418 | fails: unsorted ISO 9660 directory |
| `supergrub2-classic` (3 images) | none | 2048 | 2048 | 4 (4) | ISO 9660, bare, note naming `gpt` | 1013, 409, 410 | passes |

Forcing `apm` on the hybrids gives the following. `supergrub2-classic` selects
partition 3, `Apple_HFS`, lists 1011, 407 and 408 members, and `W` fails with "HFS
Plus bitmap marks a block nothing owns", a verdict on xorriso's volume rather than
on its map. `macbackup1` fails to open: its `Apple_HFS` partition starts at byte
2048, so the ISO 9660 probe finds the volume's second descriptor at 32 KiB of the
slice and claims it, and the partition holds classic HFS in any case.

The two `mozilla` failures are the alternate header [E10](#e10-old-volumes)
already records as left out of the image; a byte range around the volume gives the
same verdict, and neither of the two places it could lie holds a header.
`sbBlkCount` disagrees with the disk on `firefox-3.6.28` through `firefox-55.0`,
by orders of magnitude, and on the Darwin disc, which it doubles. `pmDataCnt`
equals `pmPartBlkCnt` and `pmLgDataStart` is 0 on every `Apple_HFS` and
`Apple_HFSX` entry above; they differ only on the map, driver and free entries.

## Apple source cross-check

The Apple sources of the [Sources](#sources) table were read after the
experiments, to check what the evidence and the designs rest on. They are
facts, paraphrased: file and line are given at the pinned tags so a reader can
find each one, and nothing from them is copied. Paths without a prefix are in
the `hfs` tree (`hfs-715.120.4`); `xnu/` paths are in the `xnu` tree
(`xnu-12377.121.6`, which also needed `bsd/vfs/vfs_utfconvdata.h` for the
tables). The host's `fsck_hfs` is `hfs-715.160.9` and its kernel is
`xnu-12377.161.14`: both are newer than the pinned tags, and the gap is not
closed here. A fact below that also appears in an experiment says so; where
the two differ the difference is stated.

### Name comparison table

The kernel folds a unit through one of two tables: the Latin table for units
below U+0100 (`core/UCStringCompareData.h:78`) and the main table for the rest
(`:103`); the main table's own subtable for the Latin block is never read.
The comparison routine is `core/UnicodeWrappers.c:366`. `fsck_hfs` carries its
own copy, one table for all units (`lib_fsck_hfs/dfalib/CaseFolding.h:41`,
compared by `lib_fsck_hfs/dfalib/SKeyCompare.c:153`), and the user-space
plugin of the same tree carries a third copy.

A throwaway script folded all 65536 units through each copy and through
`lib/formats/hfsplus/unicode-tables.h`. The result:

* The three Apple copies agree on all 65536 units, and all agree with the
  header: **0 differences** in 65536.
* That covers the 12146 unobservable rows, so the derivation rule there is
  confirmed by source, not only by NetBSD's witness. It covers the 30
  exception units, which fold to themselves in Apple's table.
* U+0000 folds to U+FFFF; U+FFFE and U+FFFF fold to themselves, so U+0000
  ties with U+FFFF alone, as the header says. U+2400, `:` and `/` fold to
  themselves. Exactly 16 units fold to zero (ignorable): U+200C to U+200F,
  U+202A to U+202E, U+206A to U+206F and U+FEFF, the same 16 as the header.

### Decomposition, classes and the name conversion

The conversion is `xnu/bsd/vfs/vfs_utfconv.c`; a catalog key is built with
decomposition and with escaping of illegal UTF-8 on
(`core/hfs_catalog.c:3519`).

* **Data.** Comparing `xnu/bsd/vfs/vfs_utfconvdata.h` with the header over
  the BMP: 970 decompositions, all identical to the header's; the
  decomposable set otherwise is exactly the Hangul syllables U+AC00 to
  U+D7A3 (11172), which the code computes. No Apple decomposition is longer
  than four units, none contains a decomposable unit, and none lies in
  U+2000 to U+2FFF or U+F900 to U+FAFF: the data holds no entry there, and the
  source gives no reason (the reason is TN1150's). The Hangul formula's range
  check (`:1001`) admits U+D7A4 by one, but the decomposable-unit test
  (`:116`) excludes it first, so U+D7A4 stays whole.
* **Classes.** The 297 nonzero classes of the header equal the xnu table's on
  every unit. The xnu table holds 25 more nonzero units, all assigned after
  Unicode 3.2.0: U+0350 to U+0357, U+035D to U+035F, U+0610 to U+0615, U+0656
  to U+0658, U+0CBC, U+17DD and U+1939 to U+193B. None is in the "combining"
  set that bounds a run, so they are never sorted; the header's class 0 for
  them gives the same names.
* **Algorithm.** A run is the maximal stretch of consecutive units from the
  combining set (672 units, 375 of them class 0, among them Hangul vowels and
  finals and the Indic vowel signs), counted after decomposition
  (`:392`). The run is sorted by an adjacent-swap pass that swaps only when
  the left class is larger and the right class is nonzero (`:1080`), so it is
  stable and a class-0 unit in the run is a barrier. That equals a stable
  sort of each maximal run of nonzero classes, as the design states. Nothing
  limits the run length or the length of one decomposition; the only bound is
  the 255-unit key buffer, applied after decomposition (`:439`). Thirty-six
  decompositions start with a combining unit (U+0340, U+0341, U+0343,
  U+0344, several Indic vowel signs, U+0F73 and others) and so join the run
  before them; no decomposition has more than three units after its first.
* **Surrogates.** A four-byte UTF-8 sequence becomes a pair, the high unit
  pushed through the run logic (it ends a run) and the low unit stored
  directly; neither is decomposed. A three-byte spelling of a surrogate, of
  U+FFFE or of U+FFFF, and any malformed byte, is stored as `%` and two
  uppercase hex digits per byte; `%` itself is not escaped.
* **Substitutions.** On the way in, UTF-8 `:` becomes U+002F and U+2400
  becomes U+0000 (only for non-ASCII input, after the decomposition test);
  on the way out U+002F becomes `:` and U+0000 becomes U+2400 (`:270`). A
  stored `:` also appears as `:`, so a stored U+002F and a stored `:` read
  back the same; the output conversion also refuses U+FFFE and U+FFFF.

All of this agrees with the E3 vectors and the design. The only edge the
vectors do not exercise is a run of five or more marks with more than one
class inversion; the algorithm above has no limit there.

### The illegal-name check

`fsck_hfs` rejects a file or folder key (not a thread record) whose name is
empty or over 255 units, or is `.` or `..` (`lib_fsck_hfs/dfalib/CatalogCheck.c:1350`).
The two mark pairs come from a second test (`:1905`): a repair routine for
names written before Mac OS X 10.2, under the old Unicode 2.1.2 decomposition
rules. It flags a name when (a) a unit that Unicode 3.2.0 decomposes appears
whole, (b) one of a short list of adjacent sequences appears that the old
rules wrote differently, or (c) marks are out of canonical order by
`fsck_hfs`'s own class table. The data are `lib_fsck_hfs/dfalib/DecompMakeData.c`
(raw) and `DecompData.h` (generated; checked equal for the replacement
strings).

* (c) never fires on names the writer sorts: `fsck_hfs`'s 297 classes equal
  the header's.
* (a) never fires on stored names, because the writer decomposes those units.
* (b) is the list the writer must refuse. Of its 69 entries, 44 are
  type (a) units; the other 25 are sequences of units that are stable under
  decomposition. Two of the 25, U+0FB2 or U+0FB3 followed by U+0F80 U+0F71,
  cannot be stored, because U+0F71 (class 129) sorts before U+0F80 (class
  130). That leaves **23 adjacent sequences**: U+0306 U+0307 (replaced by
  U+0310), U+0308 U+030D and U+00A8 U+030D (replaced by the same base with
  U+0301); U+030D after each of the 15 Greek letters U+0391, U+0395, U+0397,
  U+0399, U+039F, U+03A5, U+03A9, U+03B1, U+03B5, U+03B7, U+03B9, U+03BF,
  U+03C5, U+03C9 and U+03D2 (replaced by the letter and U+0301); and U+09AC
  U+09BC, U+0A21 U+0A3C, U+0B2F U+0B3C, U+0E4D U+0E32 and U+0ECD U+0EB2
  (replaced by U+09B0, U+0A5C, U+0B5F, U+0E33 and U+0EB3).
* The E3 pair probe tried every ordered pair of the 327 marks after `a`, so
  it could see only the two mark-mark pairs. The other 21 are base-then-mark
  or base-then-base and were not probed. The source says `fsck_hfs -n`
  flags all 23. The host's `fsck_hfs -d -n` was run on each of the 21 (E3,
  `fsck-sequences.tsv`) and flagged every one, exit 8, on both comparators;
  its replacement names are those the source gives. The two Tibetan sequences
  are not stored in the input order and are not flagged as stored.

### The all-ignorable name

Both the kernel (`core/hfs_catalog.c:3484`) and `fsck_hfs`
(`lib_fsck_hfs/dfalib/SKeyCompare.c:236`) order two keys by parent, then by
name, with a zero-length name compared by raw length first: the thread
record's empty name sorts before every real name of its parent. A name of
ignorable units alone has a raw length of at least one, so it goes through
the folding comparison and folds to empty. It ties only with another
all-ignorable name, and sorts before every name with a visible unit and after
the thread key of the same parent. The kernel has no special case for it, and
`fsck_hfs`'s length check counts raw units, so it passes. The source gives no
reason to refuse such a name beyond the tie with a sibling; the refusal on
`hfsplus` is the design's choice.

### Hard-link stubs

**Stub test.** Looking a record up (`core/hfs_catalog.c:838`) and listing a
directory (`:2905`) test the same things, and both apply to a *file* record:

* File link: Finder type `hlnk` and creator `hfs+`; the record's link-chain
  flag is not part of the test (it only says whether the reference is the
  node's ID or the older node number, `core/hfs_catalog.c:2921`).
* Directory link: the link-chain flag set, Finder type `fdrp`, creator
  `MACS`. Listing a directory additionally requires the reference to be at
  least 16; the lookup does not.
* Both: the record's `createDate`, converted to seconds (with the record's
  expanded-times flag), equals the root folder's `createDate` (taken at mount
  from the root record, `core/hfs_vfsutils.c:587`) **or** a second date that
  the mount takes from the *file* hard-link data folder (the `\0\0\0\0` +
  `HFS+ Private Data` folder) alone (`core/hfs_link.c:985`, `:1067`). The
  directory folder's date (`.HFS+ Private Directory Data\r`) is never used. A
  read-only mount of a volume without the file folder has only the root date.

**Stub dates.** Directory stubs carry the *file* folder's date, the same as
file stubs: both kinds are created with the one date (`core/hfs_link.c:96`).
A native directory stub does not carry the directory folder's date, so a
test against the directory folder's date would reject it whenever the two
folders' dates differ. The dirlinks capture had equal dates only because
one mount made both folders.

**Symbolic-link sources.** The kernel's link entry point returns `ENOTSUP` when the
source is a symbolic link (`core/hfs_link.c:446`); the comment gives the
`linkat` without-follow case and "for now". It is the only refusal found. The read side does not look at the indirect node's kind: a
stub resolves to whatever record the node name holds, so a stub whose node is
a symbolic link reads as one, as the design says. The source does not say
whether older systems wrote such links, and the checker has no
symbolic-link-specific rule: **not established**.

**First-link attribute.** The first-link attribute (a decimal text of the first link's
ID, with a terminating NUL, inline) is written only for *directory* hard
links, keyed by the directory node's ID: when the first link is made
(`core/hfs_link.c:253`), when a later link is made and becomes the first
(`:315`), and when the first link is unlinked and the next takes over
(`:871`; the attribute write is at `:1298`). File links keep the first-link ID in the
indirect node's catalog record and never write the attribute, which agrees
with E4, E5 and the dirlinks capture. What happens to the attribute when the
last link goes was not read.

**Directory links.** `link(2)` of a directory is refused with `EPERM` unless the
volume is journaled (`core/hfs_link.c:452`); the comment says directory links
were only *qualified* on journaled volumes, a policy, not a stated technical
need. The other conditions (`:527` to `:598`): the directory folder exists; the
source's parent and the target's parent differ; neither the target directory
nor the source's parent is the root; the source is not an ancestor of the
target; no ancestor of the target is itself a directory link; the link count
is below the limit; the source is not immutable or append-only; and there is a
free block. A directory link also sets a "has child link" flag on the
destination's ancestors and on the source's parent chain.

### Journal, volume header and wrapper

* **Replay.** A mount opens the journal when the volume's journaled attribute
  is set. If `lastMountedVersion` is `HFSJ` it does so early
  (`core/hfs_vfsops.c:1583`, `core/hfs_vfsutils.c:2491`), otherwise late,
  after the catalog is up, finding the journal file there (`:2701`); both end
  in the one open routine (`core/hfs_journal.c:1960` to `:2075`), which
  replays when the header's `start` and `end` differ. Before comparing it
  replaces a value equal to `size` with `jhdr_size` (`:1162` to `:1184`), so
  `start == size` with `end == jhdr_size` is empty. Open accepts `start` and
  `end` in `(0, size]` and multiples of 512, `size` from 256 KiB to 1 GiB; a
  wrong checksum is only logged, a wrong magic fails. A read-only mount
  needs `start == end` (`:2237`) or it refuses. The journal info block's
  `0x1` bit decides local or external, and a clear bit makes the mount look
  for an external journal device.
* **Offset base.** The info block is read at the embedded volume's start plus
  `journalInfoBlock` blocks. The early path adds the embedded start to the
  stored `offset` before opening, so it takes the stored value as *counting
  from the embedded volume's start*. The late path does the same addition and
  then *writes the sum back* (`core/hfs_vfsutils.c:2801`, `:2917`), so after
  a late mount the stored value counts from the start of the wrapper. This is
  exactly E11: the one `mkfs.hfsplus -w -J` image held 12288, then 57344 after
  one macOS mount; a stored `E + L` is thus read as `E + L` by the early path
  and as `2E + L` by the late path, and each location can hold a different
  header. The two paths therefore disagree on the base of the same
  field in Apple's own code. A second mount of that image takes the early path
  (the first mount wrote `HFSJ`) and would add the embedded start again; what
  it does then was not tried and the source does not say.
* **Alternate header.** A flush rewrites the alternate volume header
  (`core/hfs_vfsops.c:3531`, `:3840`) when the caller asks: resizing the
  volume, turning the journal on or off, and, on a journaled volume, an update of a
  system file's extents (`core/hfs_readwrite.c:5732`); and when a flush finds the extents, catalog,
  allocation or startup file's fork data modified, or the attributes file's
  (`:3752` on). An ordinary mount, unmount or sync writes only the primary
  header. The triggers that experiments left untried are thus
  known by reading, not by trial.

* **Wrapper range.** The mount computes the embedded start from the wrapper's
  first-block sector, extent start and allocation block size, and takes
  count times block size as the partition's length
  (`core/hfs_vfsops.c:1422` to `:1480`). Its only test of that range is that
  start and length are multiples of the device's logical block size
  (`core/hfs_vfsutils.c:212`). It never refuses on the difference between the
  range and the embedded volume's blocks times its block size. The difference
  picks where the alternate header goes (`core/hfs_vfsutils.c:293` to `:352`):
  spare bytes of up to one allocation block get no special handling, and the one
  alternate header stays 1024 bytes before the range's end; more than that is a
  smaller volume in a larger partition, written at both 1024 before the range's end and 1024 before
  the volume's end (`core/hfs_vfsops.c:2311` to `:2327`), and recovery reads the
  one relative to the partition (`:3596`). A volume larger than the range has
  no case of its own there: the mount does not refuse it and falls through to
  the same alternate location. So a volume no larger than the range is what it
  tolerates on purpose, and a larger one is untested rather than allowed.
  No captured wrapper has any difference at all (E10, E11).

### Compression

`xnu/bsd/sys/decmpfs.h` and `xnu/bsd/kern/decmpfs.c` fix only the
envelope: the attribute `com.apple.decmpfs`; a 16-byte header of magic
(`cmpf` as a little-endian word), type and a 64-bit uncompressed size, all
little-endian (`decmpfs.h:71` to `:140`); a whole attribute of at most 3802
bytes including the header (`decmpfs.c:561`), so at most 3786 bytes of inline
payload; type 1 is "data stored uncompressed in the attribute"; types below
255 are valid, and two dataless types with the high bit set (`decmpfs.h:102`,
`:105`) are valid for data-less files and directories only. The kernel
defines no other type: all others, including 3, 4, 7, 8, 11 and 12, belong to
the separate compression kext, which is not in the tree. **The chunk table,
the first-offset word, the resource-fork markers and any type beyond the
design's table are not established** by this source.

### Writer values and the checker

The checker (`lib_fsck_hfs/dfalib/SVerify2.c:300` to `:360`) tests a B-tree
header's depth, root number, and, on a journaled volume, that a node larger
than an allocation block lies in whole contiguous blocks. A search of the
checker found **no** test of the catalog node size beyond that, of a one-node
extents or attributes tree, of one-record index nodes, of `lastMountedVersion`
values, or of attributes bit 31 (the format header only defines the bit). The
minimum volume size and `newfs_hfs` constraints were not read. These are
**not established** by source; the native run under
[fsimg writer output](#fsimg-writer-output) is the oracle for each of them.

## fsimg writer output

`tools/hfsplus/qualify.rb` takes volumes the writer made, and one `fsimg c` made
with its defaults, through the native
cycle of the [plan](hfsplus-plan.md#9-native-qualification-and-writer-registration),
on the [host above](#macos-development-host): macOS 26.6.2 (`25G83`), with
`fsck_hfs` `hfs-715.160.9`, `hdiutil` framework and driver 683.160.3, and `ditto`
at the digests listed there. Every volume is one the tool generated in a
disposable directory, and it attaches, mounts and detaches them unprivileged.
`hdiutil info` lists none of them afterwards.

```
qualify.rb --generator BUILD/hfsplus-write-gen --fsimg BUILD/fsimg --root DIR
```

**Result: every cycle passes, on all 35 volumes.** Each one passes all seven
steps: generate, `fsck_hfs -fn` with no line of finding, the mounted compare,
the modifications, `fsck_hfs -fn` again, the remounted compare, and fsimg's own
`W`, `t --json`, `t -v` and `x -O`. No detach needed force.

What that bounds: volumes up to 1 GiB, files up to about 4 MiB (the nearly full
volumes' filler), and, separately, the one volume whose catalog passes `2^32` bytes.
Larger volumes and files are not qualified natively.

The steps, as they ran:

* Metadata compare: `find`, `lstat` and `readlink`, read-only under `hdiutil
  attach -owners on`, which needs no privilege and shows stored owners. Owner,
  group, mode, all four times, size, kind and link target match what the writer
  was given, nondefault owners and modes included; ownership needed no skip.
  Only after that does a read-only mount with owners off compare checksums,
  because the mounting user may not read a file under its stored owner.
* Modifications, read-write with owners off: a file created; `q-fragment/data`
  appended one fsynced block at a time between one-block files, 24 times; a
  folder made and one removed; a file unlinked; two attributes, of 5 and 8192
  bytes; a resource fork; a hard link; two files through `ditto
  --hfsCompression`, one inline (65536 zeros) and one in a resource fork (163840
  bytes of text). Then on files the writer stored: rename, unlink, hard link and
  attribute. Reads afterwards exclude access and change times, which those steps
  move.
* What macOS adds is left out of each mounted compare by name: `.fseventsd`,
  `.Spotlight-V100`, `.Trashes`, `.TemporaryItems` and `.DS_Store` at the root.
  `find` does not enter `.Trashes`, which refuses the mounting user. fsimg's own
  listing leaves out only those of them the last mount showed. The two private
  folders of hard links are in the catalog, but neither listing shows them, and a
  listing that did would fail, as anything else unexpected does.
* fsimg afterwards: `W` verifies the volume macOS changed, extents overflow
  records, attributes and compressed files included. `t --json` lists the
  metadata again, with `extra_data` set on exactly the attribute and resource
  fork files and on no compressed file; the `@` of `t -v` marks those same paths.
  `x -O` delivers every nonempty file's bytes, decoding both compressed ones,
  including through both names of each hard link.
* `names`: every length of 1 to 255 UTF-16 units after decomposition, spelled in
  units of one, two (`é`, a surrogate pair, a two-jamo syllable) and three to a
  character. The writer refuses each spelling's 256-unit form.
* `collisions`: E3's sets. Where the comparator makes a pair collide (always
  decomposition, Hangul, the combining order of U+0F73 and the canonical
  singletons; on `hfsplus` also case folding and ignorable units), the writer
  refuses both names together, and macOS resolves the name left out to the file
  stored under the other. Where it does not (on `hfsx` the fold and ignorable
  pairs; on both the Deseret case pair and the unordered supplementary marks),
  both names are stored and macOS resolves them to two files.
* `hfsplus-cli-defaults`: `fsimg c -F hfsplus -f IMAGE -C SOURCE .` with no other
  option, in place of the generator, from a host tree of the block tree's files,
  directories and symbolic links, since a host tree imports no FIFO. Without `-p`
  the owner is 0 and the modes are the writer's defaults; the creation and
  modification times, sizes, targets and checksums are the source's.

Block size, free blocks before and after, nodes of each tree before and after
with the leaf records after it, and the modifications refused:

| Volume | Bytes | Block | Free blocks | Extents nodes (records) | Attributes nodes (records) | Catalog nodes | Refused |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `hfsplus-block-512` | 33554432 | 512 | 65399, 65119 | 1, 2 (2) | 1, 4 (6) | 5, 20 | 0 |
| `hfsplus-block-1024` | 33554432 | 1024 | 32678, 32511 | 1, 2 (2) | 1, 4 (6) | 5, 20 | 0 |
| `hfsplus-block-2048` | 33554432 | 2048 | 16314, 16204 | 1, 2 (2) | 1, 4 (6) | 5, 20 | 0 |
| `hfsplus-block-4096` | 33554432 | 4096 | 8130, 8047 | 1, 2 (2) | 1, 4 (6) | 5, 20 | 0 |
| `hfsplus-block-8192` | 33554432 | 8192 | 4037, 3970 | 2, 2 (2) | 1, 4 (6) | 6, 18 | 0 |
| `hfsplus-block-16384` | 33554432 | 16384 | 1990, 1930 | 4, 4 (2) | 2, 4 (6) | 8, 16 | 0 |
| `hfsplus-block-32768` | 33554432 | 32768 | 967, 909 | 8, 8 (2) | 4, 4 (6) | 8, 16 | 0 |
| `hfsplus-block-65536` | 33554432 | 65536 | 455, 396 | 16, 16 (2) | 8, 8 (6) | 16, 16 | 0 |
| `hfsplus-1gib-block-4096` | 1073741824 | 4096 | 262075, 261992 | 1, 2 (2) | 1, 4 (6) | 5, 20 | 0 |
| `hfsplus-1gib-block-8192` | 1073741824 | 8192 | 131012, 130945 | 2, 2 (2) | 1, 4 (6) | 6, 18 | 0 |
| `hfsplus-smallest-empty` | 23552 | 512 | 0, 0 | 1, 1 (0) | 1, 1 (0) | 2, 2 | 8 |
| `hfsplus-smallest-tree` | 223744 | 512 | 0, 0 | 1, 1 (0) | 1, 1 (0) | 5, 5 | 12 |
| `hfsplus-nearly-full` | 4194304 | 4096 | 2, 1 | 1, 1 (0) | 1, 1 (0) | 5, 6 | 11 |
| `hfsplus-low-space` | 524288 | 4096 | 66, 0 | 1, 2 (2) | 1, 2 (3) | 5, 15 | 6 |
| `hfsplus-map-nodes` | 167772160 | 4096 | 10231, 1978 | 1, 2 (2) | 1, 4 (6) | 30722, 38905 | 0 |
| `hfsplus-names` | 33554432 | 4096 | 7941, 7626 | 1, 2 (2) | 1, 4 (6) | 245, 490 | 0 |
| `hfsplus-collisions` | 33554432 | 4096 | 8165, 8084 | 1, 2 (2) | 1, 4 (6) | 4, 16 | 0 |
| `hfsx-block-512` | 33554432 | 512 | 65399, 65157 | 1, 2 (2) | 1, 4 (6) | 5, 15 | 0 |
| `hfsx-block-1024` | 33554432 | 1024 | 32678, 32529 | 1, 2 (2) | 1, 4 (6) | 5, 15 | 0 |
| `hfsx-block-2048` | 33554432 | 2048 | 16314, 16212 | 1, 2 (2) | 1, 4 (6) | 5, 15 | 0 |
| `hfsx-block-4096` | 33554432 | 4096 | 8130, 8050 | 1, 2 (2) | 1, 4 (6) | 5, 15 | 0 |
| `hfsx-block-8192` | 33554432 | 8192 | 4037, 3968 | 2, 2 (2) | 1, 4 (6) | 6, 18 | 0 |
| `hfsx-block-16384` | 33554432 | 16384 | 1990, 1928 | 4, 4 (2) | 2, 4 (6) | 8, 16 | 0 |
| `hfsx-block-32768` | 33554432 | 32768 | 967, 907 | 8, 8 (2) | 4, 4 (6) | 8, 16 | 0 |
| `hfsx-block-65536` | 33554432 | 65536 | 455, 396 | 16, 16 (2) | 8, 8 (6) | 16, 16 | 0 |
| `hfsx-1gib-block-4096` | 1073741824 | 4096 | 262075, 261995 | 1, 2 (2) | 1, 4 (6) | 5, 15 | 0 |
| `hfsx-1gib-block-8192` | 1073741824 | 8192 | 131012, 130943 | 2, 2 (2) | 1, 4 (6) | 6, 18 | 0 |
| `hfsx-smallest-empty` | 23552 | 512 | 0, 0 | 1, 1 (0) | 1, 1 (0) | 2, 2 | 8 |
| `hfsx-smallest-tree` | 223744 | 512 | 0, 0 | 1, 1 (0) | 1, 1 (0) | 5, 5 | 12 |
| `hfsx-nearly-full` | 4194304 | 4096 | 2, 1 | 1, 1 (0) | 1, 1 (0) | 5, 6 | 11 |
| `hfsx-low-space` | 524288 | 4096 | 66, 0 | 1, 2 (2) | 1, 2 (3) | 5, 14 | 5 |
| `hfsx-map-nodes` | 167772160 | 4096 | 10231, 1978 | 1, 2 (2) | 1, 4 (6) | 30722, 38905 | 0 |
| `hfsx-names` | 33554432 | 4096 | 7941, 7626 | 1, 2 (2) | 1, 4 (6) | 245, 490 | 0 |
| `hfsx-collisions` | 33554432 | 4096 | 8160, 8079 | 1, 2 (2) | 1, 4 (6) | 4, 16 | 0 |
| `hfsplus-cli-defaults` | 253952 | 4096 | 0, 0 | 1, 1 (0) | 1, 1 (0) | 5, 5 | 12 |

`lastMountedVersion` is `fsim` before the first read-write mount and `10.0` after
it on every volume. Every volume starts with no extents or attribute records.

The four values the writer's profile takes where it departs from `newfs_hfs`. No
private folder is not a fifth: the 39 unjournaled `newfs_hfs` captures have two
catalog leaf records when formatted, the root and its thread, and macOS adds both
folders at its first mount (E2).

| Value | Outcome | Evidence |
| --- | --- | --- |
| `lastMountedVersion` `fsim` | Confirmed | `fsck_hfs -fn` and mounts, read-only and read-write, accept it on all 35 volumes |
| 4096-byte catalog nodes at every volume size | Confirmed | The `1gib` volumes at blocks of 4096 and 8192 |
| A one-node extents tree and a one-node attributes tree | Confirmed | Below 8192-byte blocks for the extents tree and 16384 for the attributes tree each tree starts at one node, and macOS grows it and inserts the records above |
| The smallest volume minimization yields | Confirmed | `smallest-empty`, 23552 bytes, and `smallest-tree` |

A volume without room is valid, and macOS cannot add to it. The verdict requires
what follows of the smallest, `cli-defaults` and nearly full volumes; of `low-space`
it requires at least one step done and one refused. The smallest
volumes and `cli-defaults` have no free block and no free catalog node, so macOS
mounts them read-write and refuses every modification, removing a stored file too:
`ENOSPC`, or `ENOTSUP` from `link(2)`, which the record takes to mean macOS had no
room for its hard-link folder at mount; the cause is inferred, not observed. The
nearly full volume has two free blocks. macOS takes one for a catalog node, refuses
the same way every step that creates a name or an attribute, and unlinks a stored
file; `fsck_hfs -fn` passes after it. `low-space` runs out part of the way through:
its 66 free blocks end at 0 with some steps done and the rest refused, and it passes
`fsck_hfs -fn`, the remount and fsimg's checks like the others. `ditto` may leave a
file uncompressed only on `low-space`, where the verdict records it; it did not.

Whether a stored file can be removed depends on the catalog's free nodes, which at
no reserve are only what rounding to a whole block leaves. `fsimg c -o cluster=B`
of one directory and three files, read-write under macOS (no cycle image covers
this), with `fsck_hfs -fn` passing after each:

| Block | Catalog nodes, free | `touch` | `rm` of a stored file |
| --- | --- | --- | --- |
| 4096 | 2, 0 | `ENOSPC` | `ENOSPC` |
| 8192 | 2, 0 | `ENOSPC` | `ENOSPC` |
| 16384 | 4, 2 | `ENOSPC` | Removes it |
| 65536 | 16, 14 | `ENOSPC` | Removes it |

A larger tree uses up the rounding: 81 empty files at 65536-byte blocks leave 7 of
the 16 nodes free.

A catalog past 32 bits, `qualify.rb --large-catalog`: 2970000 files of 240-byte
names on a volume of 4714397696 bytes, a catalog fork of 4306628608 bytes in
1051423 nodes. It is not one of the 35 volumes, and its compare steps have no
verdict: the one complete run lost its results when the tool failed to write
its log. What it establishes comes from direct checks:

| Catalog fork's `clumpSize` | `fsck_hfs -fn` on the writer's volume |
| --- | --- |
| 4294967295, `2^32 - 1` | Exit 8: `Invalid file clump size (It should be 11661312 instead of 4294967295)` |
| 4294901760, the largest multiple of 65536 | Exit 8: the same, `instead of 4294901760` |
| 11661312, the size's low 32 bits | Exit 0, `appears to be OK` |

The volume with the low 32 bits also mounts read-only, `find` lists all 2970000
files, and `fsimg W` verifies it. The first volume, with the check above failing,
also went through the modification steps of a complete run. Afterwards macOS had
extended the fork to 4703744000 bytes and written 397115392 as its `clumpSize`,
the amount it grew by, and `fsck_hfs -fn` and `fsimg W` pass on that volume. So
`fsck_hfs` also accepts a value below the low 32 bits. The rule these values
suggest is that `fsck_hfs` refuses a clump above the size's low 32 bits. It is
inferred from the two refused values and the one accepted at one fork size, and
macOS's value at another: the pinned checker source does not show where it computes
its expected value. The writer states the low 32 bits. A fork of exactly `2^32`
bytes therefore states clump 0, which no checker or kernel run has seen. Every
other volume above has a fork below `2^32` bytes, where those bits are the logical
size itself.
