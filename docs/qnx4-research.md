# QNX4 evidence and provenance

The [QNX4 format design](../specs/design-qnx4.md) is the implementation
contract. This record holds the native evidence behind it: the tools that
produced it, what each experiment showed, the structure of the PCM 3.1
navigation partition, and the answer to each question of the
[QNX4 plan](qnx4-plan.md#evidence-questions). Open work belongs in
[QNX4 qualification work](todo-qnx4.md). The material itself (the VM,
captures and the HDD image) lives outside the repository, as described in
[the QNX6 infrastructure page](qnx6-infrastructure.md).

## Research boundary

The [QNX6 research boundary](qnx6-research.md#research-boundary) applies
unchanged. The research draft
[QNX4 disk format and fsimg plan](../fsspecs/QNX4_fsimg_spec.md) ("the
draft") supplied the questions; this record answers them from native
behavior only. No Linux, qnxprobe, dissect.qnxfs or FsRover code was read
for this record. QNX 6.5.0's `dinit`, `chkfsys` and `fs-qnx4.so` were run as
external tools; their usage text was read from the binaries. No QNX code,
loader bytes or disassembly listing enters the repository. Native images
carry the proprietary QNX loader in block 1 and are not redistributable.

The H1 survey (`tools/qnx/pcm/pcmq4.py`) reports counts and histograms only:
no names, contents, identities or timestamps.

## Sources

| Key | Source | Use |
| --- | --- | --- |
| X1 | QNX 6.5.0 x86 live CD, build 201007091524 | Native formatter, checker and filesystem module, run in the QEMU VM |
| H1 | Owner's PCM 3.1 HDD, partition 3 (type `0x4f`, sectors 6313545 + 102767805) | Native populated volume, structure only |
| H3 | PCM 3.1 update disc v4.76, recovery and main IFS | Head-unit `dinit`, `chkfsys` and `fs-qnx4.so` identities |
| D | [The draft](../fsspecs/QNX4_fsimg_spec.md) | Questions and the vendor documentation it cites |

## Tool identities

QNX 6.5.0 (X1), all dated 2010-07-09 and tagged `VERSION=6.5.0`, x86:

| Tool | Bytes | SHA-256 |
| --- | --- | --- |
| `/sbin/dinit` | 24558 | `94eedb6e56a54fbae513fc00a19a17ee35883988d5659cb941bbf8950b1a1b11` |
| `/sbin/chkfsys` | 44661 | `97690308e32888a842723a7b5430402f7d9dc637368fce38dfbd60883993d156` |
| `/lib/dll/fs-qnx4.so` | 58759 | `9112874efbe38c94aa7050f77d4c07a86910785df4776eb9c05f399d04fb17d7` |

Copies are kept in `~/qnx65-vm/bin4/`. The live shell runs all three, and
`mount -t qnx4 /dev/hd0 /q4` mounts a volume on the whole IDE disk.

Head unit (H3, SH-4, stripped):

| Tool | Where | Bytes | Build mark | SHA-256 |
| --- | --- | --- | --- | --- |
| `dinit` | `~/pcm31/bin/` (from H1 partition 1 `/tools`) | 19704 | `6.3.0`, 2004-04-29 | `bb5ed873dc9389bc5901ae3d69a1365fe7016d49d464faf05356fe7ae3d1c5fd` |
| `dinit` | recovery IFS `/proc/boot` (runs `dinit -hq /dev/hd0t79`) | 22886 | `6.3.0`, 2004-04-29 | `94505e667423834adf6ae899a9065e751eaea4060c56be7fc5b7491343b1aaa7` |
| `chkfsys` | recovery IFS `/proc/boot` | 39513 | `630SP2-744`, 2007-08-24 | `319e4654d2dcc3fc90f5bf825bf7fa1450c1be93b5f902047b590445cafc2dd3` |
| `fs-qnx4.so` | main IFS `/proc/boot` | 57601 | `632-1395`, 2009-03-19 | `10119388903e9fd96cd84bd29d36de48cfa84315da4f8d1ef0bdf01e094b80c1` |
| `fs-qnx4.so` | recovery IFS `/lib/dll` | 57601 | `632-1395`, 2009-03-19 | `0614addc26ad353417b59532e66c95f4a0ee81ab1801b872f1233f49890b7561` |

Both head-unit `dinit` builds carry the same usage text and the same option
set as 6.5.0's, except 6.5.0's added `-8` (extended INT 13 loader). Neither
version has an `.inodes` size option: the draft's `-i` does not exist in
these builds. Their string tables name the same root entries (`.bitmap`,
`.inodes`, `.altboot`, `lost+found`, `.diskroot`, `.longfilenames`) and the
`IamTHE` inode signature. H1's partition 3 has exactly the 6.5.0 default
layout (below), which is the evidence that the head-unit formatter's
defaults match; the SH-4 binaries were not disassembled.

## Method

Captures are in `~/qnx65-vm/captures-qnx4/` (`FSIMG_QNX4_NATIVE`), made with
`tools/qnx/vm/capture.sh` on the 64 MiB `hd0` pre-filled with `0xAA`, and
decoded with `tools/qnx/decode/q4dump.py`. Each `NAME.txt` holds the guest
commands and full output. Population recipes are `tools/qnx/guest/pop4.sh`.
`q4-manifest-NAME` records the native manifest of capture `NAME`: the image
loaded back onto `hd0`, mounted read-only and listed by
`tools/qnx/guest/manifest4.sh` (`ls -ld` of every path in sorted order and
`cksum` of every regular file).

* **Whole-disk volumes** use `dinit -hq /dev/hd0` (131072 blocks).
* **Exact block counts** use a regular file: `dinit -hq -S BYTES FILE` on a
  QNX4 scratch volume mounted from `hd1` (the RAM `/tmp` holds only about
  2 MiB), checked with `chkfsys -m` on the file, then copied to `hd0` with
  `dd`. Read such a capture as a slice of `BLOCKS * 512` bytes.
* **The non-repairing check** is `chkfsys -m -f -u [-s] [-x] TARGET </dev/null`:
  `-m` for an unmounted target, `-f` no fixes, `-u` ignore the clean flag,
  `-x` detailed exit codes. A clean volume prints `NEW bitmap is equal to
  OLD bitmap` and exits 0. A finding prints a description and a prompt that
  fails on the null input; with `-x` the exit status is nonzero (`0x30` for
  a block in use but free in the bitmap, `0x60` for unrepaired corruption).
  `chkfsys -m` derives the expected bitmap length from the target's size,
  so the target must be exactly the volume: a 4095-block volume checked on
  the 64 MiB disk fails with "Root directory has wrong first extent block".

| Capture | Experiment |
| --- | --- |
| `q4-empty-131072-default`, `-dinitonly` | E1, default `dinit -hq` on the whole disk, with and without a following `chkfsys` |
| `q4-empty-{1000,4095,4096,4097,5001,8192,8193,131071}-file` | E1, exact block counts |
| `q4-empty-131072-label{A,B,C}` | E2, `-l` and `-L` |
| `q4-pop-131072-default` | E3, population |
| `q4-grow-131072-default` | E4, directory growth |
| `q4-frag-131072-default` | E5, interleaved appends of 2 to 122 blocks |
| `q4-inodes-131072-default` | E6, 200 long-named files |
| `q4-long-131072-default` | E7, names over 48 bytes |
| `q4-mounted-131072-default` | E8, captured while mounted after `sync` |
| `q4-zeroloader-131072-default` | E9, zeroed block 1 |
| `q4-free-131072-default` | A freed `.inodes` slot left unreused; content written to `.boot` and `.altboot` |
| `q4-over-131072-default` | E10, appends and truncation |

## Observations

### Volume layout (E1)

For a volume of `N` blocks, `dinit` writes, in 1-based block numbers, with
`B = ceil(N / 4096)`:

| Blocks | Content |
| --- | --- |
| 1 | Loader (the native IPL for `-h`) |
| 2 | Root block |
| 3 .. 2+B | Bitmap |
| 3+B .. 10+B | Root directory, 8 blocks, size 4096 |
| 11+B .. 26+B | `.inodes`, 16 blocks, size 8192 |
| 27+B .. 42+B | `lost+found` directory, 16 blocks, size 8192 |

Every other block is free. The smallest volume `dinit` accepts is 43 blocks
(42 refuses: "too small to initialize"); at 43 blocks no block is free. The
draft's four-block root directory does not match this formatter.

### Root block (E1, E2)

Block 2 holds five 64-byte inode records, a label and zeros:

| Offset | Record | Name field | Mode | Status | Links | Extent |
| --- | --- | --- | --- | --- | --- | --- |
| `0x000` | root directory | `/` | `040775` | `0x21` clean, `0x01` mounted | 2 + subdirectories | 3+B, 8 |
| `0x040` | `.inodes` | empty | `0100444` | `0x11` | 1 | 11+B, 16 |
| `0x080` | `.boot` | empty | `0100600` | `0x11` | 1 | none |
| `0x0c0` | `.altboot` | empty | `0100600` | `0x11` | 1 | none |
| `0x100` | `lost+found` | empty | `040755` | `0x11` | 2 | 27+B, 16 |
| `0x140` | zero | | | | | |
| `0x1c0` | volume label, 48 bytes, NUL-padded | | | | | |
| `0x1f0` | zero | | | | | |

All four times of every record equal the format time; uid and gid are 0;
bytes `0x3a` to `0x3e` are zero. `-l LABEL` writes the label at `0x1c0`;
`dinit -hq -L LABEL` rewrites the label (`-L` without `-h` refuses the hard
disk, as `labelB` records); a 67-byte label is truncated to 48 bytes and
followed by 16 zero bytes (`labelC`). Without `-l` the field is zero.
Writing `.boot` and `.altboot` through the mounted filesystem sets their
records' status to `0x13` (`q4-free`). Record 1 stays `0x11` on every
populated capture, including `q4-inodes`, whose `.inodes` grew, and on H1.

### Root directory slots (E1, E3)

| Slot | Record |
| --- | --- |
| 0 | `.`: link record, status `0x08`, target `(2, 0)` |
| 1 | `..`: link record, status `0x08`, target `(2, 0)` |
| 2 | `.bitmap`: direct record, `0100444`, status `0x01`, extent `(3, B)`, size `ceil(N / 8)` |
| 3 | `.inodes`: link record to `(2, 1)` |
| 4 | `.boot`: link record to `(2, 2)` |
| 5 | `.altboot`: link record to `(2, 3)` |
| 6 | `lost+found`: link record to `(2, 4)`, status `0x04`, and `0x08` at byte `0x36`, inside the long-name pointer, in every empty capture |
| 7 | zero, status `0x02` |
| 8 | `.longfilenames`: direct record, `0100444`, status `0x01`, size 0, no extent |
| 9 to 63 | zero |

Slot 6's status has neither the used nor the link bit, so `ls`, the native
directory listing and lookup (`ls /q4/lost+found` fails) do not see it, yet
the root's link count counts it, `chkfsys` walks it ("Dir
/dev/hd0/lost+found") and the bitmap allocates its blocks. Slot 7 is an
ordinary free slot that `fs-qnx4.so` reuses for the next entry.

### Dot entries (E1, E3, E4)

A `.` record is the name `.`, a NUL, the bytes `I`, `0x03`, `Q`, `N`, `X`
at offsets 2 to 6, a zero at 7, a 32-bit little-endian value at 8, and zeros
to offset 47; its target is the directory's own record position; status
`0x08`. The value at 8 is the directory's first-extent length in blocks: 8
for the root and for `mkdir` on the 131072-block disk, 2 for `mkdir` on a
4096-block volume, 16 for `lost+found`; it is unchanged
when a directory grows. A `..` record is the name `..` with a zero name tail and
targets the parent's record position: `(2, 0)` for a child of the root,
otherwise the position of the parent's own record in its parent (a direct
slot, or an `.inodes` slot when the parent has a link name). `lost+found`'s
`..` alone targets `(3+B, 0)`, the root's `.` slot. Both records have zero
spare bytes and long-name pointer.

### Records, names and kinds (E3, E7)

* **Direct records** hold names of 1 to 16 bytes, unterminated at 16. Their
  status is `0x03` (used, modified) once file data were written and `0x01`
  for directories, symbolic links, FIFOs and untouched system files.
* **Names of 17 to 48 bytes** are link records (status `0x08`, spare bytes
  zero, long-name pointer 0) whose target is a used `.inodes` slot, status
  `0x13` or `0x11`. Directories with such names live in `.inodes` too (H1).
* **Names of 49 to 505 bytes** are link records whose 48-byte name is the
  first 38 name bytes, `~` and eight lowercase hex digits of the long-name
  block, then a zero byte; the pointer at link offset `0x35` is that physical block. The block
  in `.longfilenames` holds the directory block (32 bits), the slot (8
  bits), state `0x01`, the full name NUL-padded in 505 bytes and a zero last
  byte. The first long name grows `.longfilenames` from 0 to 16 blocks; a
  freed record is reused. Two names sharing their first 48 bytes therefore
  never collide. A 506-byte name is refused. `mv` to a long name failed in
  the live shell (`stat() failed ... Filename too long`); in the
  qualification runs a native `mv` of a 49-byte name to a 70-byte one
  succeeds and checks clean (fsimg writer output below), while the shell
  cuts a 505-byte path to 255 bytes, so renaming the longest names is
  unobserved.
* **Hard links** turn every name into a link record to one `.inodes` slot
  holding the link count. Unlinking back to one name leaves the link record
  and the `.inodes` slot.
* **Deleted slots keep their bytes** with the used and link bits cleared
  (`hk2` keeps its name and target with status `0x00`). Free slots carry no
  information.
* **`.inodes` slot 0** is the name `IamTHE.inodeFILE` and the 32-bit value
  `0x00100000` at offset 16, the rest zero, also on H1. A used slot's name field is 16 zero bytes.
  Never-used slots are zero; a slot freed by `rm` keeps its bytes with
  status `0x00` (`q4-free`), so unused slots carry no information. H1 has
  no nonzero unused slot. `.inodes` grows by a 32-block extent (E6).
* **Kinds:** symbolic links are `0120777` with the target as content
  (`ln -s` relative, absolute, dangling and to a directory); FIFOs are
  `010664` with size 5120 and 10 allocated blocks the guest never writes;
  native `ls -ld` reports its size as 0 (`q4-manifest-pop`), while a
  directory's listed size is its stored size.
  A FIFO edited on the host to size 0 with no extent and its blocks freed
  checks clean, lists as a FIFO and checks clean again after a write to the
  volume (`~/qnx65-vm/writer-captures/q4-probe-fifo0-4096`);
  `mknod` is absent from the live CD. The type byte at `0x3e` and the bytes
  `0x3a` to `0x3d` were zero in every record.
* **Metadata:** `chown 65535` fails with `EINVAL`; 65534 is stored. `touch
  -t` stores mtime and atime from 0 up to 2147483647 and leaves creation
  time and ctime; modes include `0000` and setuid.
* **Link counts:** a directory has 2 plus its subdirectories; the root counts
  `lost+found`.

### Extents (E4, E5, E6)

On the 131072-block disk a directory is created with, and grows by, 8-block
extents; on a 4096-block volume `mkdir` allocates 2 blocks (size 1024, `.`
value 2), so the step depends on the volume size. The root directory is 8
blocks at every measured size. `.inodes` grows by 32. An extent block holds
the next block (0 at the end), the previous block (0 in the first),
`entries_used` (8 bits), three zero bytes, the sum of its extent lengths, up
to 60 extents, `IamXblk` and a NUL at `0x1f0`, and at `0x1f8` a copy of the
record's first extent in the first block only (zero in later blocks). A
record's extent count includes its own extent: 61 extents need one extent
block, 62 and 121 two, 122 three. Interleaved one-block appends fragment a
file into one extent per append; sequential writes stay contiguous. The
extent count at record offset `0x30` is 16 bits, so one object has at most
65535 extents in at most 1093 extent blocks.

### Bitmap (E1, H1)

`.bitmap` is a direct record of size `ceil(N / 8)` bytes over `B` blocks
starting at block 3. Bit `k` (LSB first) is block `k + 1`. Bits from `N` to
the end of the last used byte are set (`0x80` at 4095 blocks, `0xfe` at 4097
and 5001), and the bytes from the size to the end of block `2 + B` are
`0xff`. So `N` is the slice length, and `ceil(N / 8)` equals the size.
At the largest `N`, 2^31 blocks, the bitmap is 524288 blocks (256 MiB).

### State flags (E8)

The root record's status is `0x21` after `dinit` and after `umount`, and
`0x01` while mounted, after `sync`. No other record carried the busy bit.

### Loader (E9)

With block 1 zeroed, `chkfsys` finds nothing, the volume mounts, accepts a
new file and checks clean again.

### Allocation beyond EOF (E10)

After appends, a truncating rewrite and `umount`, every regular file's
allocation is exactly `ceil(size / 512)` blocks, with and without the
`overalloc` option. FIFOs (above) are the exception.

### H1 partition 3

102767805 blocks; `.bitmap` 12845976 bytes over 25090 blocks with set
padding bits; root directory at 25093 (`3 + B`), `.inodes` at 25101 and
`lost+found` at 25117, all with the default lengths, so the head-unit
formatter's layout matches 6.5.0's. The root status is `0x01`: the volume
was not cleanly unmounted. The loader block is nonzero and differs from
6.5.0's in 249 bytes. The label and bytes `0x140` to `0x1bf` are zero.
`.longfilenames` exists and is empty.

| Measure | Value |
| --- | --- |
| Directories | 21 visible plus `lost+found`; depth at most 2; modes `040700`, `040777`; one directory has 20 subdirectories |
| Regular files | 50 of mode `0100666` plus the system files; 43743973 data blocks |
| Extents | every object a single extent; no extent blocks |
| Names | 76, all ASCII and valid UTF-8; direct 5 to 16 bytes, link 17 to 31 bytes; none over 48 |
| `.inodes` | 128 slots, 48 used, including directories |
| Link records | 51, all status `0x08`, long-name pointer 0, spare zero; no object has two names |
| Statuses | regular `0x01`, `0x03`, `0x11`, `0x13`; directories `0x01`, `0x11`; type byte 0 |
| Free slots | 21 with stale bytes (status `0x00`, and the `lost+found` slot `0x04`) |
| Allocation | every object exact, nothing past EOF; reachable allocation equals the bitmap exactly |
| Full verification | `fsimg verify` complete: 43744183 blocks allocated, each with one owner, and 59023622 free; no orphan, no link-count or occupancy mismatch, so allocation past EOF needs no wider reader policy |
| Owners | all uid and gid at most 65534; reserved bytes zero |

## fsimg writer output

`test-qnx4-write --native` compares fsimg's empty volumes with the `dinit`
captures of the same geometry (the whole 131072-block disk with and without
labels, and 1000, 4095, 4096, 4097, 5001, 8192, 8193 and 131071 blocks):
every byte from block 2 to the end matches. Block 1 differs only because
fsimg writes no loader (E9).

`tools/qnx/guest/qualify4.sh` ran the native cycle on volumes made with
`test-qnx4-write --create`, loaded onto `hd0` by `capture.sh -i`; captures
are `~/qnx65-vm/writer-captures/q4-writer-NAME-qualify`, whose records name
each image's SHA-256. The cycle is `chkfsys -m -f -u -s -x`, a read-write
mount, a manifest (`ls -ld` and `cksum` of every path), native writes, a
second check, a remount, a third manifest and a third check. Native writes
append 70 interleaved blocks to one file (70 extents through two extent
blocks), create a directory holding files named with 33 and 65 bytes,
`mkdir` and `rmdir` a directory, rename a file to a 31-byte name and delete
the interleaving spacer.

| Image | Blocks | Content | Result |
| --- | --- | --- | --- |
| `empty0` | 43 | Empty catalog, no free block | Three clean checks; mount and read (no native writes possible) |
| `tree0` | 262 | Fixture tree at its smallest volume, no free block | Three clean checks; mount and read |
| `tree4096` | 4096 | Fixture tree, 1 bitmap block | Full cycle clean |
| `tree8192` | 8192 | 2 bitmap blocks | Full cycle clean |
| `tree12288` | 12288 | 3 bitmap blocks | Full cycle clean |
| `tree131072` | 131072 | The default geometry, label `PCMnav` | Full cycle clean |
| `full131072` | 131072 | Nearly full: 256 free blocks | Full cycle clean |

The fixture tree holds names of 16, 17, 48, 49 (two sharing their first 48
bytes) and 505 bytes, a directory and a FIFO and a symbolic link with link
names, nested directories, a setuid directory owned by 1000:65534, mode
`0000`, a non-ASCII name, `.boot` with 700 bytes of content, an empty
`.altboot` and `.diskroot`. In every capture the native manifest equals
fsimg's writer manifest in name, kind, mode, link count, owner, size,
symbolic-link target and `cksum` (27 paths for the tree, `.bitmap`,
`.inodes` and `.longfilenames` excepted because fsimg hides them). fsimg's
`verify -d` and `ls` then read every captured image back after the native
writes, including the native 70-extent file and native long names: all
verify clean.

`q4-writer-tree4096-longnames` exercises fsimg-written long names: native
`rm` of a 49-byte name and `mv` of the other 49-byte name to a 70-byte name
pass `chkfsys`; QNX frees the two old records (state `0x00`) and writes a
new one, and fsimg verifies the result. `mv` of the 505-byte name fails in
the guest shell because the path it passes is cut to 255 bytes ("Source
file not found"); this is the utility's path limit, not the volume's.

Native `ls` without `-l` omits some names over 48 bytes from a directory
listing, including a 505-byte name QNX itself created
(`q4-long505-4096-file`), while `find`, shell globbing, `stat` and `cat`
see them. The native comparisons therefore use `find` and `ls -ld` of each
path. On a 4096-block volume the first long name grows `.longfilenames` by
2 blocks rather than 16, as directories grow by smaller steps there.

### PCM navigation geometry

On a sparse 52617116160-byte `hd1` (`FSIMG_QNX_HD1`), QNX 6.5.0's `dinit
-hq` at H1 partition 3's 102767805 blocks writes the same root block,
`.inodes`, `lost+found` and fixed root slots as H1, apart from times. H1
differs only where use changed it: the root's link count and status
(`0x01`, not clean), the user entries from root slot 7 on, and the loader
block, which is a different QNX loader build. So the head unit formatted the
partition with the 6.5.0 defaults.

`test-qnx4-write --create DIR tree 102767805` writes the fixture tree at
that geometry as a sparse file (25351 allocated blocks, 12 MB on disk) in
96 seconds, most of it the test's own full-image check. Attached as `hd1`
with the VM stopped while it was written, it passes the native cycle
(`q4-writer-pcmnav-102767805-qualify`): three clean checks, the native
manifest equal to fsimg's, and fsimg's `verify -d` of the result clean.

The partition's compatibility manifest, the fields an image for it must
reproduce; the namespace counts are those of the H1 table above:

| Field | Value |
| --- | --- |
| Geometry | `N` 102767805 blocks of 512 bytes, `B` 25090, root directory at 25093 for 8 blocks, `.inodes` at 25101 for 16, `lost+found` at 25117 for 16 |
| Profile | long names, `.longfilenames` present, label zero; the head unit's loader is nonzero, fsimg's zero |
| Namespace | 22 directories with the root, depth at most 2, 50 regular files, no symbolic link, FIFO or second name; names 5 to 31 bytes, none over 48; owners at most 65534 |
| Not captured | Disk composition: the partition table and the other partitions |

## Evidence questions

| Question | Answer |
| --- | --- |
| Block 2 bytes `0x100` to `0x1ff`; label | `lost+found` record at `0x100`, zero to `0x1bf`, label `0x1c0` to `0x1ef`, zero to `0x1ff` (E1, E2) |
| Special records | Root block table above (E1, H1) |
| Root slot order; dot entries; signature offset | Root slot table and dot entries above; signature at name offset 2 (E1, E3) |
| `.bitmap` size and padding | `ceil(N / 8)`; padding bits and bytes set (E1, H1) |
| `.inodes` default, slot 0, used and free slots | 16 blocks; slot 0 signature and `0x00100000`; used `0x11` or `0x13`; free zero (E1, E3, E6) |
| `.longfilenames` default, `-N` | Created empty by default, `0100444` root-owned (E1, H1); `-N` omits it and leaves slots 7 and 8 zero (`q4-empty-131072-N`); `-R` puts an empty `0100444` `.diskroot` in slot 8 and `.longfilenames` in slot 9 (`q4-empty-131072-R`) |
| Extent block header | Extents section above (E5, E6) |
| Directory size and growth | A multiple of 512 equal to the allocation; 8 blocks per step on the 64 MiB disk, 2 on a 4096-block volume; root always 8 blocks; `lost+found` 16 (E1, E4, H1) |
| Link counts | Directories 2 plus subdirectories; hard links in the `.inodes` slot (E3, H1) |
| Busy, modified and clean flags | Clean is root status `0x20`; modified `0x02` marks written data; busy not observed (E8, H1) |
| Zero loader | Accepted by `chkfsys` and the mount (E9) |
| Long-name pointer and record | Physical block; record layout and mangled prefix above (E7); native rename to 70 bytes and delete check clean, 505-byte rename unobserved (shell limit) |
| Allocation beyond EOF | Not retained for regular files; FIFOs own unwritten blocks (E10, E3) |
| `.inodes` growth | 32-block extents through an extent block (E6) |
| FIFO and device encodings | FIFO above; devices open (no `mknod`) |
| uid, gid and time limits | uid and gid up to 65534; times 0 to 2147483647 via `touch` (E3) |
| Head-unit `dinit` defaults | Same option set, and H1 has exactly 6.5.0's layout at its geometry (PCM navigation geometry); not disassembled |
| Name encodings on H1 | All ASCII; none outside the profile, so owner decision 6 needs no byte-name policy |

Open: device nodes and the head-unit modules' acceptance of fsimg output. They block no reader feature;
their consumers are listed in [the TODO](todo-qnx4.md).
