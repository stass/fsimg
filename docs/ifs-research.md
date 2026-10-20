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
  `tools/qnx/decode/ifsflip.py` builds such a disk of mutated copies. Larger
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
| `ifs-tools`, `ifs-tools-mkxfs` | Tool binaries for the identities above |

## Observations

### Header (E1, E2, E9)

* The ImageFS header flags are `0x04` little-endian and `0x05` big-endian
  (`+bigendian`) in every native image; no other value occurs.
* The mountpoint is `/` by default, the `mount=` value otherwise; `mount=`
  with an empty value stores an empty mountpoint, as H3's `PCM3_IFS2` has.
  `dir_offset` is the mountpoint's NUL rounded up to 4 (`0x5c` for `/`,
  `0x60` for `/foo`).
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
| Implicit directories | 22 | 16 |
| Inode flag nibbles | `0x0`, `0x8`, `0xc`, `0xe` | the same |
| Duplicate paths (link record before file record) | 1 | 3 |
| Zero-length files | 1 | 0 |

In all of them the mountpoint is `/`, `boot_ino[0]` is `0xe0000002` and
names exactly one regular file record by its full inode word, `script_ino`
is 3 and names one file the same way, flagged members are 4-byte aligned and
none page-aligned, every name is valid UTF-8, and depth is at most 4.

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

The payload is one unframed LZO1X stream decoding to a nonbootable ImageFS
of 68849668 bytes: flags `0x04`, empty mountpoint, no boot or script
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

## Owner decisions from this evidence

* **Duplicate paths.** The last record with a path is the entry, as
  `dumpifs -x` leaves it; earlier records with that path are shadowed:
  counted by info, reported by verification, never delivered. `procnto`'s
  own rule is unknown until a boot test (E12) runs; a different result
  reopens this.
* **Odd paths.** A stored path with a `.` or `..` component, an empty
  component, a leading slash, or a parent that is not a directory makes
  the image `FSIMG_EUNSUPPORTED`: `mkifs` writes such paths but fsimg does
  not represent them.

## Evidence questions

| Question | Answer | Evidence |
| --- | --- | --- |
| Root record; explicit versus implicit directories; metadata of implicit parents | The root is always the first record (empty path, inode 1, `040755`, time 0 with `-n`). Only `type=dir` names get records. No native reader shows implicit parents' metadata (no mount); fsimg gives them the explicit-directory record's values: `040755`, uid 0, gid 0, time 0 | E1, E3, S1 |
| Header flags, mountpoint default, `dir_offset` padding, mount flags | `0x04` and `0x05`; `/`; NUL rounded up to 4 with unchecked contents; zero | E1, E2, E9, S1 |
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
| Whether the x86 startup accepts uncompressed and LZO bodies; startup fields that change on growth; offset-dependent processed members; raw members | Open: E8 and E12 are not run. They feed only the wrap (stage 11) and owner decision 6 | |
| How to boot an x86 image in QEMU | Open: E12. The live CD's El Torito entry is a 2.88 MB floppy image with its own loader, so replacing `boot/fs/qnxbase.ifs` in a rebuilt ISO is the first candidate | |

## Open evidence

* E8 and E12, before the wrap stage.
* `dumpifs -u` copies of the eight H3 boot images, through the `big.img`
  route, before the decoder stage's gate.
* The four unidentified `hbcifs` header fields, before `hbcifs`
  verification can be complete.
* Images larger than 16 MiB built natively: the guest's RAM disk limits the
  experiments to small bodies; S1 covers bodies up to 68849668 bytes.
