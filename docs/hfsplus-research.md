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
more.

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

Status: open.

### E2 Population

Status: open.

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
