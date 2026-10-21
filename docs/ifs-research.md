# QNX IFS evidence and provenance

The [IFS plan](ifs-plan.md) orders the work and the IFS format and driver
designs are the implementation contract. This record holds the native
evidence behind them: the tools that produced it, the transport between host
and guest, what each experiment showed, the structure of the PCM 3.1 and
QNX 6.5.0 boot images, and the answer to each
[evidence question](ifs-plan.md#evidence-questions). The material itself
(the VM, captures, oracle decodes and member manifests) lives outside the
repository, as described in
[the QNX6 infrastructure page](qnx6-infrastructure.md).

## Research boundary

The [QNX6 research boundary](qnx6-research.md#research-boundary) applies
unchanged. The research draft
[QNX IFS interoperability specification](../fsspecs/QNX_IFS_fsimg_spec.md)
("the draft") supplied the questions; this record answers them from native
behavior. No `lclevy/dumpifs`, `qnxprobe`, `qnx_dumpers`, LZO or UCL source
code was read for this record. QNX 6.5.0's `mkifs` and `dumpifs` were run as
external tools; their usage text and string tables were read from the
binaries. GPL `liblzo2` and `libucl` were called through their public APIs
only, by the out-of-tree oracle below. No QNX code, startup bytes, member
file or member name enters the repository.

The surveys (`tools/qnx/decode/ifsdump.py` without `--records`) report
counts and histograms only: no names, contents, identities or timestamps.

## Sources

| Key | Source | Use |
| --- | --- | --- |
| X1 | QNX 6.5.0 x86 live CD, build 201007091524 | Native `mkifs` and `dumpifs` in the QEMU VM; `boot/fs/qnxbase.ifs`, `qnxbasedma.ifs`, `qnxbasesmp.ifs` |
| H3 | PCM 3.1 update disc v4.76, `HEADUNIT/ADR*/` | Eight SH-4 boot images (`PCM3_IFS1*`, `PCM3_Emergency*`) and the `hbcifs` image `PCM3_IFS2.ifs`; structure only |
| D | [The draft](../fsspecs/QNX_IFS_fsimg_spec.md) | Questions and the vendor documentation it cites |

## Tool identities

QNX 6.5.0 (X1), x86, all dated 2010-07-09 and tagged `VERSION=6.5.0`
(`use -i`). `/bin/mkifs` is a symbolic link to `mkxfs`.

| Tool | Bytes | SHA-256 |
| --- | --- | --- |
| `/bin/mkxfs` (`mkifs`) | 262620 | `61ee3186d4bbe20184505cb9e16edd5f6ce4d85c6b101abe7a494a5568919a86` |
| `/bin/dumpifs` | 32745 | `d3795dc96db449b301dd779da1cd120d2a1846398ee5f132e43c98c9dabbc568` |
| `/bin/mkifsf_elf` | 20651 | `fcdaf8736c2f3c937e18f11c305b1b1174c66628f4d1553b623d6ba8628e4fb2` |
| `/boot/sys/startup-bios` | 793685 | `0effe7b1198f39b9e23b72d77eab141496f368a71779058f08298b575b319244` |
| `/boot/sys/procnto` | 872512 | `ff7280f883638f2b1c8ac3d7d81a8e9becda07c5a53f08b7ab4731fd9cb9c7a0` |

Copies are kept in `~/qnx65-vm/bin-ifs/`. `dumpifs` takes `-b`, `-m` (MD5 of
each member), `-u FILE` (an uncompressed copy), `-v`, `-x`, `-f FILE` and
`-z`. `mkifs` takes `-l`, `-n` and `-nn`, `-p`, `-r`, `-s` and `-v`. There
is no `mount_ifs`.

Buildfile attributes found by trial (`ifs-basic`, `ifs-data`, `ifs-extra`):
`mount=`, `prefix=`, `type=file|link|fifo|dir`, `uid=`, `gid=`, `perms=`,
`dperms=`, `+page_align`, `+raw`, `+bigendian`, `compress=1|2|3`,
`+compress` and `image=` are accepted; `mtime=`, `mountpoint=` and
`type=chr` are refused ("Unknown attribute", "Invalid file type").

## Oracle

`tools/qnx/oracle/oracle.c` is a black-box codec oracle: it calls
`lzo1x_decompress_safe`, `ucl_nrv2b_decompress_safe_8` and the matching
compressors through the libraries' public APIs and knows the boot-image
chunk framing. `tools/qnx/oracle/build.sh` builds it against Homebrew's
`lzo` 2.10 and `ucl` 1.03 into `~/.local/share/fsimg-oracles/ifs-codecs/`;
the binary combines GPL libraries, so it never enters the tree or the fsimg
build. Its decodes are in `~/qnx65-vm/captures-ifs/decoded/`.

Two witnesses check the oracle. Every decoded ImageFS's additive checksum is
zero and its length equals the startup header's `imagefs_size`. For X1
`qnxbase.ifs`, the ImageFS part of native `dumpifs -u` output has the same
POSIX `cksum` as the oracle's decode, 2731401004 over 3296204 bytes
(`ifs-s2-qnxbase-u2`). The H3 boot images exceed the serial transport's
budget, so their `dumpifs -u` comparison is open (below).

## Method

Captures are in `~/qnx65-vm/captures-ifs/` (`FSIMG_IFS_NATIVE`), made with
`tools/qnx/vm/capture.sh` on the 64 MiB `hd0` pre-filled with `0xAA`.

* **Building.** `tools/qnx/guest/popifs.sh GROUP` builds one group of
  experiment images in the guest's `/tmp` (a 4 MiB RAM disk) and writes each
  to `hd0` at a 512 KiB slot. Each case prints a
  `CASE slot name rc bytes cksum` line, its buildfile, `mkifs` output and
  `dumpifs -v`. Large intermediates go to `/dev/shmem`, which is RAM-backed
  and not size-limited like `/tmp`.
* **Out of the guest.** The host cuts each case out of the capture's disk
  image by slot offset and length and compares the guest's `cksum`; every
  case of every capture matched. Cut cases are kept in
  `~/qnx65-vm/captures-ifs/cases/CAPTURE/`.
* **Into the guest.** `capture.sh -i IMAGE` loads a sector-padded host image
  onto `hd0` through `load.py` (encoded size at most 3 MiB), and the guest
  command copies each case out with `dd` by offset and length.
  `tools/qnx/decode/ifsflip.py` builds such a disk of mutated copies; the
  E10 disk's twelve uncompressed copies are cut from `inputs/e10.img` by
  the slot offsets and lengths of `inputs/e10.cases` into
  `cases/ifs-e10-sense/`, named `slotNN-CASE.ifs`; `test-ifs --native`
  expects every copy but `slot00-unchanged.ifs` to fail verification. Larger
  images, such as the H3 boot images, need the stopped-VM `big.img` route of
  the QNX6 infrastructure, which this record has not used.
* **Decoding.** `tools/qnx/decode/ifsdump.py` dumps an uncompressed boot
  image or ImageFS (survey by default; `--records` and `--manifest` print
  member names and belong outside the tree), and `ifschunks.py` reports a
  compressed boot image's chunk framing without decoding.

| Capture | Experiments |
| --- | --- |
| `ifs-basic` | E1 minimal and reproducibility, E2 header, E4 kinds, E5 metadata, E9 byte order |
| `ifs-names` | E3 component lengths, depth, odd paths, parents |
| `ifs-extra` | E3 path and link-target limits, non-ASCII names; E7 LZO, UCL and big-endian LZO on a 109304-byte body |
| `ifs-data` | E6 sizes, identical contents, alignment, duplicates, a file below a file; E7 zlib |
| `ifs-e10-sense` | E10 `dumpifs` sensitivity, through `capture.sh -i` |
| `ifs-e3-extract` | `dumpifs -x` of the odd-path and duplicate images |
| `ifs-s2-qnxbase-u`, `-u2` | `dumpifs -u` of X1 `qnxbase.ifs` against the oracle |
| `e12-links` | E12 boot of records below a symbolic link: buildfiles, scripts, captures and serial transcripts |
| `ifs-tools`, `ifs-tools-mkxfs` | Tool binaries for the identities above |

## Observations

### Header (E1, E2, E9)

* The ImageFS header flags are `0x04` little-endian and `0x05` big-endian
  (`+bigendian`) in every native image; no other value occurs.
* The mountpoint is the `mount=` value; `mount=` with an empty value stores
  an empty mountpoint, as H3's `PCM3_IFS2` has. Without `mount=`, a
  nonbootable image stores an empty mountpoint (`e1-one-nn` matches
  `e2-mount-x` byte for byte) and a bootable one `/` (E12's `cAr.ifs`), the
  value of every H3 and X1 boot image. `dumpifs -v` names a nonempty
  mountpoint only. `dir_offset` is the mountpoint's NUL rounded up to 4
  (`0x5c` for `/` and for the empty mountpoint, `0x60` for `/foo`).
* The bytes between the mountpoint's NUL and `dir_offset` are zero in every
  6.5.0 image but not in H3: every H3 image has one to three nonzero bytes
  there. Padding is therefore not a closed rule.
* Mount flags, `chain_paddr` and the ten spare words are zero everywhere.
  `boot_ino[]` and `script_ino` are zero in nonbootable images.
* Nonbootable output ends exactly at `image_size`, which is a multiple of 4.
  The additive checksum over the whole ImageFS is zero in every image.
* `-n` zeroes the root record's time; `-nn` zeroes every time. Two builds
  with the same option are byte-identical.

### Record table (E1, E4, E6)

* The records tile `[dir_offset, hdr_dir_size - 4)`, and four zero bytes end
  the table, in every image. Every record length is a multiple of 4, every
  extended-attribute offset is zero, and no inode word is zero.
* The first record is always the root: a directory with the empty path,
  inode 1, mode `040755`. An empty buildfile produces only the root.
* Directories have records only when the buildfile names them with
  `type=dir`; every other parent is implicit. Directory records carry mode
  `040755` and time 0 whatever `perms=` says (`dperms=` sets the mode).
* Inodes are numbered from 2 in buildfile order. The table lists records
  other than regular files first, in reverse inode order, then regular
  files; neither the table nor the data is in path order.
* Record tails (after the payload's NUL) are zero in 6.5.0 images; in H3 the
  root record's three tail bytes are nonzero. Gaps between file data are
  zero everywhere.

### Kinds (E4, E5)

| Kind | Mode | Payload after the 24-byte prefix | Path at |
| --- | --- | --- | --- |
| Directory | `040755` | none | `+0x18` |
| Regular file | as `perms=`, default `0664` | u32 data offset, u32 length | `+0x20` |
| Symbolic link | `0121777` from the buildfile (H3: `0121555`, `0120777`, `0120555`) | u16 `sym_offset`, u16 `sym_size` | `+0x1c` |
| FIFO | `010666` | u32 0, u32 0 | `+0x20` |

Devices, named specials and sockets cannot be produced (`type=chr` is
refused). uid and gid take the full 32 bits; `perms=` takes every value from
`0000` to `7777`. File times come from the host file's time.

### Symbolic links (E4, S1)

In every link of every image, the target starts at
`path + sym_offset` with `sym_offset = path_length + 1`, is `sym_size` bytes,
and is followed by a NUL, then zero padding. Relative, absolute, dangling,
`..` and non-ASCII targets all follow it. Targets up to 1024 bytes are
accepted; 4000 bytes is refused ("File name too long").

### Names (E3)

* A path up to 1024 bytes is accepted and 1025 is refused; one component may
  use the whole length (no 255-byte component limit). Depth 200 is accepted.
* Names are stored as given: UTF-8 and Latin-1 bytes alike. A space in a
  buildfile name is refused by the buildfile parser.
* `mkifs` stores `x/../y` and `emp//ty` verbatim and strips a leading `./`
  and leading slashes. It accepts a path below a regular file (`d` and
  `d/x`) and two records with one path.
* `dumpifs -x` writes `x/../y` to `y` (the guest resolves the `..`), fails
  with status 1 on `emp//ty` ("No such file or directory") and on `d/x`
  ("Not a directory"), never creates symbolic links, and for two files with
  one path leaves the later record's bytes.

### Data (E6)

* Data follows the table at `hdr_dir_size`. Ordinary members are packed
  without alignment (offsets such as `0xc7`); `+page_align` places a member
  on a 4096-byte boundary. A zero-length file's offset is the next data
  offset.
* Identical contents are stored twice: `mkifs` does not share ranges, and
  no range overlaps in any native image.

### Compression (E7, S1, S2)

* `compress=2` (LZO) and `compress=3` or `+compress` (UCL) on a nonbootable
  buildfile produce a startup header with no startup code: version 1,
  `flags1` `0x08` or `0x0c`, machine 0, `startup_size` 260 (the header and
  a 4-byte startup checksum), followed by the chunk framing.
  `+bigendian compress=2` writes a big-endian startup header.
* `compress=1` (zlib) fails ("Error writing compression file") and leaves a
  260-byte header with `stored_size` 0, so 6.5.0 produces no zlib body.
* Chunks are framed by 16-bit big-endian stored lengths and a zero length.
  Zero padding follows the terminator up to `stored_size - 4`, and a 4-byte
  stored checksum ends the image. `stored_size` is a multiple of 4.
* Decoded chunk sizes: every non-final chunk is 65536 bytes, except that the
  H3 `PCM3_IFS1` family has pairs of 4096 and 61440. The smallest non-final
  decoded size observed is 4096; the largest stored chunk is 65059.
* The additive sum over the startup section, and over the whole stored image
  from the startup header to `stored_size`, is zero in every image, so the
  stored body's own sum is zero too.

### `dumpifs` sensitivity (E10)

`dumpifs -v` prints the stored checksum word but checks neither checksum. A
flipped byte in a record name, link target, file data, table terminator or
either trailer, a byte-swapped trailer, and out-of-range header or data
offsets all exit 0 with an ordinary listing; a flipped mode prints
"Unknown type"; a corrupted LZO chunk prints "Early end reading image
trailer". Its "Early end reading directory" line appears on almost every
well-formed nonbootable image. `dumpifs` is therefore a listing and
extraction oracle only; checksum verification has no native oracle.

### Mounting (E11)

6.5.0 has no `mount_ifs`, and `procnto` mounts only the image it boots. A
nonbootable image has no native runtime reader.

### Paths below a link at boot (E12)

**Method.** Everything is in `e12-links/`; the scripts ran from a scratch
copy of it and name that directory, so a rerun first edits their `cd`
lines. The guest's `mkifs` builds each image in `/dev/shmem/e12`
(`build.sh`) from a buildfile that starts with the attribute
`[linker="ntox86-ld -T/lib/nto.link %(h!=0, -Ttext 0x%t%)%(d!=0, -Tdata 0x%d%) -o%o %i %[M -L%^i -uinit_%n -lmod_%n%]"]`,
boots `[virtual=x86,bios +compress]` (or without `+compress`) with
`startup-bios -D8250.3f8.115200 -v` and `procnto -v`, and marks its members
`[data=copy]`. `capn.sh` prints each buildfile and `dumpifs -v`, writes the
images to `hd0` at 2 MiB slots and captures the disk; `cut.py` cuts them
out on the host and the guest's `cksum` matched. `mkfd.py` writes each
`bios.boot`-prefixed image over the boot image inside a copy of the live
CD's El Torito 2.88 MB floppy (the boot catalog's image at ISO LBA 31,
extracted as `eltorito.img`), at byte 22528, which the floppy's own loader
reads; the new image must fit the old one's `0x440 + stored_size` bytes.
`bootfd.sh` boots one floppy in a separate headless
`qemu-system-i386 -accel tcg -m 256 -drive file=fd.img,format=raw,if=floppy -boot a -serial file:ser.txt -display none -no-reboot`;
the startup's `-D8250.3f8.115200` puts the console on COM1, so the boot
script's `display_msg` lines land in the serial file, and the script waits
for its final marker (`E12-DONE`, 120 seconds at most) before stopping
QEMU. Without `[data=copy]`, a program from the image starts once and
every later start fails with
`Unable to start "cat" (2)` (`v5`, `b2`), so a script that runs a tool
twice needs it.

**Cases.** `c0` is the control. `cL` holds a link `L -> /some/dir` (an
existing directory holding the file `x`), a link `L/a` and a file `L/b`;
`cM` the same with the link last in the buildfile; `cN` a link
`N -> /nowhere` with `N/a` and `N/b`; `cD` a directory record `D` with a
link `D/a` and a file `D/b`; `cP` a link with nothing below it; `cA` all of
them in one image. `mkifs` placed `L/a` before `L` and `L/b` after it,
`M` before both `M/a` and `M/b`, and `N/a` before `N`. `cAu` is `cA`'s
buildfile without `+compress` (`flags1` `0x01` against `0x0d`), and `cAr`
is `cAu` with every record after the root reversed in the table by
`rev.py`, which keeps every record's bytes and alignment, so both
checksums are unchanged.

**Results** (`ser-*.txt`, `out-*.txt`).

* `ls -la /L` and `ls -ld /L` show only the link,
  `L -> /some/dir`; the root lists `L`, `M` and `N` as links and `D` as a
  directory.
* `ls -la /L/a`, `ls -la /L/b`, `cat /L/a` and `cat /L/b` report
  `No such file or directory`, as do the same paths below `M` and `N`.
* `cd /L` succeeds and lists `/some/dir`'s own `x`; `cd /N` fails with
  `No such file or directory`. The records stored below a link never
  appear, neither below the link nor in its target.
* Below the directory `D`, `D/a` and `D/b` list, open and read normally.
* `cA`, `cAu` and `cAr` give the same answers line for line: the result
  depends neither on the table order nor on compression, and an
  uncompressed image boots through the same floppy loader (its banner is
  `Loading IFS...done` instead of `Loading IFS...decompressing...done`).
* `dumpifs -v` lists the records below a link as ordinary table rows.

So `procnto` shadows every record whose path lies below a symbolic-link
record. Records below a regular file or a FIFO were not booted.

**Side observations, not investigated.** Implicit parents show different
times: `usr` shows the build time in `c0`; `some` and `usr` show time 0 in
`cA` and `cAu` and the build time in `cAr`; the explicit directory `D`
shows its record's time 0 throughout. The early `b1` and `b2` attempts,
whose scripts ran tools without `[data=copy]` or started a serial driver,
are kept for completeness.

### S1: H3 boot images

All eight have the startup header at offset 0: version 1, `flags1` `0x09`
(virtual, LZO), `header_size` 256, machine 42 (SH), `startup_size` 53512,
`preboot_size` 0, six nonzero bytes in the header tail, and the stored image
ending at the end of the file. `ram_size = startup_size + imagefs_size`.

| Property | `PCM3_IFS1` family (6) | `PCM3_Emergency` (2) |
| --- | --- | --- |
| Chunks | 278 to 294 | 120 |
| Records | 206 to 230 | 174 |
| Kinds (directory, file, link) | 1, 153 to 177, 52 | 1, 131, 42 |
| Implicit directories | 21 | 16 |
| Inode flag nibbles | `0x0`, `0x8`, `0xc`, `0xe` | the same |
| Duplicate paths (link record before file record) | 1 | 3 |
| Records below a non-directory | 2 link records below one top-level link, both before it in the table | 0 |
| Depth (components) | at most 6 | at most 4 |
| Zero-length files | 1 | 0 |

Comparing the startup sections of `PCM3_IFS1`, `PCM3_IFS1_Macan` and
`PCM3_Emergency` byte by byte locates what varies. Between the two
`PCM3_IFS1` variants only `ram_size`, `stored_size`, `imagefs_size` and the
section's last word differ, so the startup checksum is the last 4 bytes of
the startup section. `PCM3_Emergency` also differs in the header word at
`0x44` and in the NUL-separated startup command line embedded near the end
of the section (`-n48M -v -C1200,0` against `-m256M -n48M -v`); both follow
the startup build, not the ImageFS.

In all of them the mountpoint is `/`, `boot_ino[0]` is `0xe0000002` and
names exactly one regular file record by its full inode word, `script_ino`
is 3 and names one file the same way, flagged members are 4-byte aligned and
none page-aligned, and every name is valid UTF-8. `ifsdump.py`'s survey
reports 22 implied directories for the `PCM3_IFS1` family because it
counts the top-level link, a parent of the two records below it; the
derived tree has 21.

`PCM3_IFS2.ifs` begins with a 64-byte `hbcifs` header:

| Offset | Value | Meaning |
| --- | --- | --- |
| `0x00` | `hbcifs\0\0` | Identity |
| `0x08` | u32 LE 68849668 | Decoded length |
| `0x0c` | u32 LE 33575140 | Payload length, the file length minus 64 |
| `0x10` | u32 LE 32 | Unidentified |
| `0x14` | u32 LE `0x33ea4eb6` | Unidentified; neither CRC-32 nor the additive sum of the payload |
| `0x18` | `02 01 01 00` | Unidentified |
| `0x1c` | `49 4f 33 f3` | Unidentified |
| `0x20` | 32 zero bytes | |

The payload is one unframed LZO1X stream that ends exactly at the end of the
file (the oracle's safe decoder consumes all of it) and decodes to a
nonbootable ImageFS of 68849668 bytes: flags `0x04`, empty mountpoint, no boot or script
references, 336 records (2 directories, 214 files, 120 links), inode
nibbles `0x0`, `0x8` and `0xc`, nonzero header padding, data starting on a
page boundary above `hdr_dir_size`, and no duplicate paths.

### S2: X1 boot images

`qnxbase.ifs`, `qnxbasedma.ifs` and `qnxbasesmp.ifs` have a 1088-byte BIOS
prefix with `preboot_size` 1088, version 1, `flags1` `0x0d` (virtual, UCL),
machine 3, `startup_size` 75056, a zero header tail, 51 chunks (50 of 65536
decoded bytes), and the `ram_size` relation. Each ImageFS has 45 records
(1 directory, 40 files, 4 links), 2 implicit directories, zero header
padding, and the same boot and script references as H3. `dumpifs -u`
copies the image with its startup header unchanged (the compression value
stays set) and the body decoded.

## fsimg writer output

`tools/qnx/guest/qualifyifs.sh` ran the native cycle on the images
`test-ifs-write --create` builds; `tools/qnx/decode/ifsqual.py pack` put
them on one `hd0` disk, loaded by `capture.sh -i`, and `ifsqual.py
compare` judged the record. The capture is
`~/qnx65-vm/writer-captures-ifs/ifs-writer-qualify`, with the images and
manifests in `ifs-writer-qualify-cases/`; its record names the disk's
SHA-256, and the guest's `cksum` of every one of the 97 members matched the
host's. The images are not reproducible: every entry without a time of its
own, every directory included, carries the planning clock, so each image
has nonzero directory times. For each image the cycle is:

1. `dumpifs -v` and `dumpifs -m`, compared with the manifest the
   independent checker of `test-ifs-write` decodes: every record in table
   order with its offset, size, name, link target, owner, mode, inode and
   time, the header and table geometry and the mountpoint, and with the
   image itself for every file's MD5 and the trailer word;
2. `dumpifs -x` into an empty directory on a QNX4 scratch volume (a 2 MiB
   file on the RAM `/tmp`, mounted for the run), then `ls -ldn` and `cksum`
   of every path, compared with the manifest's size, cksum, mode and time
   of every file and with the extraction of the native image of step 3;
3. `mkifs -nn` of the same tree from a generated buildfile (`type=dir` for
   every directory, every owner and mode given, the contents copied off the
   disk), then steps 1 and 2 on that native image, whose listing must agree
   with fsimg's in every name, kind, size, owner, mode and MD5, apart from
   the `01000` bit `mkifs` adds (below);
4. no mount: 6.5.0 has none for a nonbootable image (E11).

| Image | Records | Content | Result |
| --- | --- | --- | --- |
| `one` | 2 | One 9-byte file | All three steps pass |
| `names` | 218 | Components of 1, 48, 49, 255, 256 and 505 bytes; two-, three- and four-byte UTF-8; a space; names differing by case; depth 201; a 991-byte path | All three steps pass |
| `limits` | 515 | A 992-byte path, a 1024-byte component and path, depth 512 | Steps 1 and 3 pass with 14 names past `dumpifs`'s record buffer; `dumpifs -x` stops with status 1 on both images at a path past the buffer, which it names with unrelated memory: on fsimg's at the depth-512 file ("Invalid argument"), on the native image at the 1024-byte name ("Filename too long") |
| `kinds` | 23 | Files of 0 to 33, 5000, 65537 and 70001 bytes, a zero source, nested and empty directories, relative, absolute, `..`, dangling, non-ASCII and 1024-byte link targets, a FIFO | All three steps pass; the 1024-byte target lies past the record buffer |
| `metadata` | 19 | Owners 0, 65535 and 2^32 - 1; modes 0000, 0644, 0755, 4755 and 7777 on files, 0000, 0700 and 7777 on directories, 0755 on a link, 0600 and 7777 on FIFOs; times 0, 2^31 - 1 and 2^32 - 1 on files and directories, 2^32 - 1 on the link, 0 and 2^31 - 1 on the FIFOs | All three steps pass |
| `data` | 11 | Files of 0, 1, 3, 4, 65535, 65536 and 65537 bytes, two with the same contents, a zero-filled file | All three steps pass |
| `empty` | 1 | The root only | All three steps pass |
| `mount-empty` | 2 | One file, the empty mountpoint | All three steps pass |
| `mount-long` | 2 | One file, a 1024-byte mountpoint | All three steps pass |
| `big-endian` | 23 | `kinds` in big-endian order | All three steps pass |

`dumpifs` accepts every image: each `-v` and `-m` run exits 0 and prints
nothing beyond its own `Early end reading directory` line, which it prints
for the native images too. Nonzero directory times, which `mkifs` never
writes, list as stored and leave `dumpifs -x` unaffected: it applies no
directory metadata. That meets owner decision 7's registration bar.

What the cycle showed about the native tools, for fsimg's and the native
images alike:

* `dumpifs` reads each record into a 1024-byte buffer and prints its path,
  and a link's target, only as far as the record's first 1024 bytes:
  1000 bytes of a directory's path, 992 of a file's or FIFO's, and 996 of
  a link's path, its NUL and its target together. Past that it prints
  unrelated memory, so `dumpifs -x` fails on such a file with "Invalid
  argument" or "Filename too long". The longest file path it shows whole
  is 991 bytes. It prints at most 512 bytes of the mountpoint.
* `dumpifs` prints the trailer word in little-endian order whatever the
  image's byte order.
* `dumpifs -x` creates no directories (an image path below a missing
  directory fails, "No such file or directory"), no symbolic links and no
  FIFOs, stops at the first failure with status 1, and restores each
  file's mode and time but not its owner.
* `mkifs` sets the `01000` bit on every file and link with an execute bit,
  whatever `perms=` says, and only `dperms=` sets a directory's mode.
* The guest's `rm -r` stops at 50 levels unless given `-l`.

## Owner decisions from this evidence

* **Duplicate paths.** The last record with a path is the entry, as
  `dumpifs -x` leaves it; earlier records with that path are shadowed:
  counted by info, reported by verification, never delivered. `procnto`'s
  own rule is unknown: E12's boot method can test it, and a different
  result reopens this.
* **Odd paths.** A stored path with a `.` or `..` component, an empty
  component or a leading slash makes the image `FSIMG_EUNSUPPORTED`:
  `mkifs` writes such paths but fsimg does not represent them.
* **Paths below a non-directory.** A record whose path lies below the
  entry of a symbolic link, a regular file or a FIFO is shadowed, exactly
  like an earlier record of a duplicate path, whatever the table order;
  the non-directory is delivered. E12 shows `procnto` doing so for
  symbolic links; files and FIFOs were not boot-tested, and the owner
  chose the same rule for them.

## Evidence questions

| Question | Answer | Evidence |
| --- | --- | --- |
| Root record; explicit versus implicit directories; metadata of implicit parents | The root is always the first record (empty path, inode 1, `040755`, time 0 with `-n`). Only `type=dir` names get records. No native reader shows implicit parents' metadata (no mount); fsimg gives them the explicit-directory record's values: `040755`, uid 0, gid 0, time 0 | E1, E3, S1 |
| Header flags, mountpoint default, `dir_offset` padding, mount flags | `0x04` and `0x05`; empty when nonbootable, `/` when bootable; NUL rounded up to 4 with unchecked contents; zero | E1, E2, E9, E12, S1 |
| Table terminator; zero-length and zero-inode records | Four zero bytes at `hdr_dir_size - 4`; no zero-length or zero-inode record occurs | E1, S1 |
| Nonbootable output ends at `image_size`; padding contents | Yes; padding is not a closed rule (H3 has nonzero header padding and record tails) | E1, E6, S1 |
| Inode numbering, flag nibble, boot and script matching | Buildfile order from 2; nibbles `0x0`, `0x8`, `0xc`, `0xe`; references match one record's full inode word | E1, S1 |
| Conventions versus free choices | Conventions: root record first, implicit parents, 4-byte record padding, the table terminator, data after the table. Free choices: inode numbering, record order, data order and alignment | E1, E6, S1 |
| Name and path limits; non-ASCII; record length | Path at most 1024 bytes, no component limit below it; bytes stored as given; records stay far below 65535 | E3 |
| Symbolic-link layout | `sym_offset = path_length + 1`, target NUL-terminated, in every case | E4, S1 |
| FIFO, device, named-special, socket encodings | FIFO: two zero words; the others cannot be produced | E4 |
| uid, gid, mode and time limits; `-n`, `-nn` | 32-bit ids, all permission bits; times from host files; `-n` and `-nn` above | E5 |
| Duplicate content, shared ranges, hard links | No sharing by `mkifs` and none in native images; duplicate paths occur (owner decision above) | E6, S1 |
| Extended attributes | Never used | E1 to E9, S1 |
| Compression values, chunk sizing, padding after the terminator | LZO and UCL, not zlib; 65536-byte chunks with a smallest observed non-final 4096; zero padding to a 4-byte `stored_size` | E7, S1, S2 |
| Startup and stored checksum domains | Additive, over the startup section and over the whole stored image | E7, S1, S2 |
| Header tail variant | Opaque: zero in X1, six nonzero bytes in H3 | S1, S2 |
| Big-endian bodies | Produced natively by `+bigendian`, nonbootable and with LZO | E9, E7 |
| Whether `dumpifs` checks checksums | No | E10 |
| Native mount of a nonbootable image | None | E11 |
| `hbcifs` header fields and checksum | Identity, decoded length and payload length identified; four fields open | S1 |
| Whether the x86 startup accepts uncompressed and LZO bodies; startup fields that change on growth; offset-dependent processed members; raw members | Uncompressed and UCL bodies boot. The rest is open: E8 is not run. It feeds only the wrap (stage 11) and owner decision 6 | E12 |
| How to boot an x86 image in QEMU | The image replaces the boot image inside a copy of the live CD's El Torito floppy, a headless QEMU boots it, and `display_msg` markers in the COM1 serial file detect the boot | E12 |
| How `procnto` resolves a path below a symbolic-link record | Through the link; the records stored below it are unreachable in every table order tested | E12 |

## Open evidence

* E8, and E12's remaining boots (each E8 image, a raw member, a
  duplicated path), before the wrap stage.
* `dumpifs -u` copies of the eight H3 boot images, through the `big.img`
  route, before the decoder stage's gate.
* The four unidentified `hbcifs` header fields, before `hbcifs`
  verification can be complete.
* Images larger than 16 MiB built natively: the guest's RAM disk limits the
  experiments to small bodies; S1 covers bodies up to 68849668 bytes.
