<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com> -->

# APFS research and oracles

Facts about the documentation, native tools and external programs that
establish fsimg's APFS work. The external programs are development oracles:
none is linked into, copied into or required by fsimg, and nothing from them
enters the tree. Captures, specimens, logs and raw tool output stay outside
the repository ([plan](apfs-plan.md), stage 0).

## Sources and licenses

### Apple File System Reference

* Title *Apple File System Reference*, author Apple Inc., published at
  <https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf>.
  181 pages. The revision printed on every page footer, and the latest entry
  of its Revision History, is 2020-06-22; the PDF's creation date agrees
  (2020-06-22, producer xdvipdfmx 20170318). That entry added the Sealed
  Volumes chapter.
* SHA-256 `74c3db584971aea2b11549ca25799ead824e1bd8994e94cd3f8665ce5aea79cc`,
  532193 bytes.
* Local copy outside the repository:
  `~/.local/share/fsimg-oracles/apfs-reference/Apple-File-System-Reference.pdf`,
  with its text extraction `apfs-reference.txt` beside it, made by
  `pdftotext -layout`. The document is Apple's copyrighted publication, kept
  as a reading copy and never redistributed; the format designs paraphrase
  its layout tables.

### Oracles

Each is built by `build-aux/build-apfs-oracles.sh TOOL [new-directory]` into
`~/.local/share/fsimg-oracles/TOOL` by default. The script creates the
directory exclusively, fetches the pinned input, refuses a fetched revision
or archive that is not the pinned one, and installs the programs into `bin/`
of that directory.

| Tool | Source | License | Pinned input |
| --- | --- | --- | --- |
| `apfsprogs`: `apfsck`, `mkapfs` | <https://github.com/linux-apfs/apfsprogs> | GPL-2.0, by its `LICENSE` file; the source files carry no SPDX tags | git revision `b9c4cb404f88ffab6c4f260bbab29a8fb4784907`, a revision after the newest tag `v0.2.1` (`acabdd5f22d2d2c8c73509f333bd571ae882fa60`, 2025-06-02) |
| `libfsapfs`: `fsapfsinfo` | <https://github.com/libyal/libfsapfs> | LGPL-3.0-or-later, by `COPYING.LESSER` and the file headers | release archive `libfsapfs-experimental-20261030.tar.gz`, SHA-256 `c727d23197af3f2c9e39618545c4719366633a666725b83be2a3214a48a11df5`, from tag `20261030` (commit `782a81b737ebda214226d61586d27bd80d47ad3c`) |
| `apfs-fuse`: `apfs-dump`, `apfs-dump-quick`, `apfsutil` | <https://github.com/sgan81/apfs-fuse> | GPL-2.0-or-later, by the file headers and `LICENSE`; its `3rdparty/lzfse` submodule is BSD-3-Clause | git revision `66b86bd525e8cb90f9012543be89b1f092b75cf3`, submodule `lzfse` at `e634ca58b4821d9f3d560cdc6df5dec02ffc93fd` (the gitlink at `3rdparty/lzfse`; the same commit `lib/codec/lzfse/` records in [references](references.md#lzfse-and-lzvn-decoders)) |

* `libfsapfs` is taken from the release archive because its git tree omits
  the libyal support libraries and the generated `configure`; `synclibs.sh`
  would fetch those from their moving heads. The archive carries the
  versions the release was tested with.
* The `apfs-fuse` mount program is not built. Its three offline tools need
  no FUSE library.
* The oracles are built and run only. Their source is not read for
  implementation, and none is copied.

## Tool identities

### Host

* `sw_vers`: macOS 26.6.2, build 25G83; Intel Xeon W-3245 (x86_64).
* Apple clang 21.0.0 (clang-2100.1.1.101), CMake 4.4.4.

### Native Apple tools

`/sbin/newfs_apfs` and `/sbin/fsck_apfs` are symbolic links into
`/System/Library/Filesystems/apfs.fs/Contents/Resources/`. Both are universal
Mach-O files (x86_64 and arm64e) signed as `com.apple.newfs_apfs` and
`com.apple.fsck_apfs`. Neither has a `--version` option, `what` prints no
version string, and each prints its usage when run without arguments, so
the identity is the macOS build above and the SHA-256 of the executable:

| Tool | SHA-256 of the executable |
| --- | --- |
| `newfs_apfs` | `6b79996b14d381e2a96597de9b3a80cb38d902c46c09f3a3e216c4d4f25d99fe` |
| `fsck_apfs` | `cb766c122d2412c745ed39ba71e12cc6425b4976c7f213557828695edd0ba429` |

`fsck_apfs` records the formatter's version in each volume it checks:
`The volume PROBE was formatted by newfs_apfs (2811.160.7.0.4)`. That is
the `newfs_apfs` version string of this macOS build.

`newfs_apfs` usage: `[-o options] [-b block-size] [[-s volume-size] | [-r
volume-reserve] [-q volume-quota]] [-v volume-name] [-i | -e] [-U uid] [-G
gid] [-E [-S passphrase | -W]] [-A | -C] [-R role] [-D] [-w] [-x] [-T] [-c]
[-a] device`. `fsck_apfs` usage: `[[-q | -n | -y] [-r num] [-l] [-s] [-W]
[-S] [-o] [-b num] [-c] [-C] [-d] [-E path] [-F | -M] [-g | -x] [-X] [-T
[-D]] ] device`.

### Oracles

| Tool | Version output | Builds on |
| --- | --- | --- |
| `apfsck`, `mkapfs` | `apfsck -v`: `apfsck b9c4cb4`; `mkapfs` has no version option, usage `[-L label] [-U UUID] [-u UUID] [-F tier2] [-B tier2-blocks] [-sv] device [blocks]` | Linux only (Ubuntu 24.04, gcc 13.3.0, in the `build-aux/linux-check.sh` image). On macOS the build stops at `#include <linux/types.h>` in `include/apfs/types.h`; the oracle is not built there and no header shim is substituted. `apfsck` takes a regular file or a block device |
| `fsapfsinfo` | `fsapfsinfo 20261030` | macOS 26.6.2 with Apple clang, and the Linux image. Configured without gettext, OpenSSL, zlib, FUSE and Python: the local DEFLATE and AES-XTS copies in the archive serve, and the macOS binary links only `libSystem` |
| `apfs-dump`, `apfs-dump-quick`, `apfsutil` | None; each prints its usage: `apfs-dump [-map mapfile.txt] file.img output.txt`, `apfs-dump-quick <main-device> [-f fusion-device] <Logfile.txt>`, `apfsutil [device]` | macOS 26.6.2 with CMake 4.4.4 and `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`, because the upstream `cmake_minimum_required(VERSION 3.0)` is refused by CMake 4. Not built on Linux: the image has no CMake, and the script refuses before creating a directory. It links the system zlib and bzip2 |

The oracles run on a 64 MiB image that `newfs_apfs` formatted, as the
procedure below makes (`f.img`):

| Command | Result |
| --- | --- |
| `fsapfsinfo f.img` | One volume `PROBE` with the case-insensitive incompatible feature and the hard-link-map compatible feature, and no snapshots |
| `apfsck f.img` | Exit 0 and no output |
| `apfs-dump-quick f.img q.txt` | Exit 0; a 1854-line dump naming the checkpoint object maps and the volume |
| `apfs-dump f.img out.txt` | Exit 0 and a 2465-line dump; it then prints `Dumping device 1` and `Aborting` (the absent fusion tier) |

`apfsck` does not run on macOS, and a bind-mounted file is not a regular
file inside the colima container. The Linux builds therefore run the script
in the image that `build-aux/linux-check.sh` builds, with the network enabled
because `linux-check.sh` itself runs offline. The colima VM shares only the
home directory, so the image to check goes under it first:

```sh
# img: the untagged image ID that linux-check.sh prints as
# "Successfully built ID" (`docker images -a` lists it afterwards).
cp f.img ~/scratch/f.img
docker run --rm --user 1000:1000 \
    -v "$PWD/build-aux/build-apfs-oracles.sh:/s.sh:ro" \
    -v ~/scratch:/probe:ro "$img" sh -c '
        sh /s.sh apfsprogs /tmp/o && cp /probe/f.img /tmp/f.img &&
        /tmp/o/bin/apfsck /tmp/f.img'
```

## Native procedure

All steps run unprivileged on the development host and need no
administrator rights. Verified on the macOS build above in a scratch
directory on an external volume:

```sh
mkfile -n 64m f.img                      # sparse file; 8 blocks allocated
dev=$(hdiutil attach -nomount \
    -imagekey diskimage-class=CRawDiskImage f.img | awk '{print $1; exit}')
newfs_apfs -v PROBE "$dev"               # exit 0, no output
fsck_apfs -n "$dev"                      # exit 0; "The container ... appears to be OK."
hdiutil detach "$dev"                    # "diskN" ejected.
```

* The raw attach prints the device node (`/dev/disk6`) and nothing is
  mounted: `-nomount` stops the system from mounting, and the synthesized
  container disk appears as a second device (`/dev/rdisk7s1` is the volume
  `fsck_apfs` reports).
* `fsck_apfs -n` checks the container superblock and checkpoint, the space
  manager and its free queue trees, the container and volume object maps,
  the snapshot metadata tree, the fsroot tree, the extent reference tree and
  the allocated space, and ends `The container /dev/diskN appears to be OK.`
* After `hdiutil detach` the image file holds the formatted container; it
  is sparse, so its allocated size is far below its length.

## Experiments

Stage 1 fills this section ([plan](apfs-plan.md), stage 1). Captures live
in `FSIMG_APFS_NATIVE`, on the development host
`/Volumes/Optaine/fsimg-apfs/native`, one directory per capture as
`tools/apfs/capture.rb` names it, with the dumper's output beside the
image: `apfsdump.json` from `tools/apfs/apfsdump.rb --json` and
`apfsdump.txt` from the plain form.

### E1 Empty containers

**Procedure.** On the host above, `capture.rb --kind newfs --option plain`
at 372736 bytes (the minimum), 4 MiB, 32 MiB, 512 MiB, 1 GiB, 8 GiB and
100 GiB, and `capture.rb --kind hdiutil --option plain` at 32 MiB. Each
capture is `apfs-KIND-BYTES-plain`, for example
`apfs-newfs-1073741824-plain`. Every capture but the minimum ends
`fsck_apfs -n` with "appears to be OK" and exit 0, and every dump reports
no problem. The 4 MiB capture is added to the plan's list because it is
the smallest container `fsck_apfs` opens.

A size sweep ran the same steps without `fsck_apfs` at 90 more block
counts and kept only the dump, in
`$FSIMG_APFS_NATIVE/e1-sweep/nBLOCKS/apfsdump.json`: 92 to 100, 128, 200,
256, 300, 400, 512, 700, 900, 1000, 1023, 1100, 2000, 4000, 4097, 4544,
4545, 5000, 6000, 9088, 9089, 10000, 13632, 13633, 16000, 24000, 32767,
32769, 40000, 65535, 65537, 100000, 131071, 131073, 200000, 262143,
262145, 300000, 900000, 1048575, 1048577, 1163264, 10000000, 11000000,
12000000, 12320768, 40000000, 50000000, and `2^k` and `3 * 2^(k-1)` for
`k` from 10 to 26.

**Minimum size.** Bisection on sparse files, 4096 bytes at a time:

| Blocks | `newfs_apfs` |
| --- | --- |
| 82 and below (1, 8, 32, 64, 77, 80, 81 and 82 tried) | Exit 73: `nx_format:176: spaceman initialization failed: 28 - No space left on device` |
| 83 to 90 (83, 88 and 90 tried) | Exit 73; at 83 `omap initialization failed`, at 90 `newfs_apfs: unable to format volume inside /dev/diskN: No space left on device` |
| 91, 92 | Exit 0, but the container holds no volume: one checkpoint at XID 1, an empty object map (apfs-newfs-372736-plain) |
| 93 and above | Exit 0 with the volume |

`fsck_apfs -n` refuses every container below 1024 blocks with `error:
device /dev/rdiskN failed to open with error: Result too large`, exit 66
(bisected: 1023 refused, 1024 accepted). [A1]'s 1 MiB minimum, the
reader's, lies between: the reader refuses the 91- to 255-block containers
`newfs_apfs` makes, and `fsck_apfs` refuses them too.

**Two checkpoints.** Every container with a volume holds two checkpoints.
XID 1 (descriptor slots 0 and 1, data slots 0 and 1) maps only the space
manager (OID 1024) and the reaper (1025); its free queues have tree OID
zero. XID 2 (slots 2 and 3, data slots 2 to 5) maps the space manager, the
reaper and the two free-queue roots (1027 and 1029), and the container
object map resolves the volume superblock, OID 1026; the next OID is 1030.
Descriptor slots past the newest checkpoint are all zero. The objects XID 2
rewrote carry XID 2; chunk-information blocks, chunks and bitmaps it did
not touch keep XID 1 (in apfs-newfs-8589934592-plain, 63 of 64 chunks).
`fsck_apfs -n` accepts the XID-1 state alone: a copy of
apfs-newfs-4194304-plain with descriptor blocks 3 and 4 zeroed and block 0
replaced by block 2, the XID-1 superblock, checks "the checkpoint with
transaction ID 1" and appears OK (`/Volumes/Optaine/fsimg-apfs/e1/probe-x1.img`
and `probe-x1.fsck`). The XID-1 values this section quotes for containers
with a volume were read from the older checkpoint's raw blocks; the bisection
logs are `/Volumes/Optaine/fsimg-apfs/e1/bisect-results.log`.

**Container values per capture** (XID 2 unless noted; all areas
contiguous):

| Capture | `N` | Descriptor | Data | Pool ring | Pool | Chunks, `C` | `L`, `I`, `M`, `F` | Free blocks |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| apfs-newfs-372736-plain (XID 1) | 91 | 1 + 8 | 9 + 52 | 61 + 16 | 77 + 6 | 1, 1 | 1, 1, 1, 1 | 6 |
| apfs-newfs-4194304-plain | 1024 | 1 + 8 | 9 + 52 | 61 + 16 | 77 + 6 | 1, 1 | 1, 1, 1, 1 | 931 |
| apfs-newfs-33554432-plain | 8192 | 1 + 8 | 9 + 124 | 133 + 16 | 149 + 6 | 1, 1 | 3, 1, 3, 1 | 8027 |
| apfs-hdiutil-33554432-plain | 8192 | 1 + 8 | 9 + 124 | 133 + 16 | 149 + 6 | 1, 1 | 3, 1, 3, 1 | 8027 |
| apfs-newfs-536870912-plain | 131072 | 1 + 8 | 9 + 388 | 397 + 16 | 413 + 15 | 4, 1 | 29, 1, 8, 1 | 130634 |
| apfs-newfs-1073741824-plain | 262144 | 1 + 16 | 17 + 992 | 1009 + 16 | 1025 + 27 | 8, 1 | 116, 1, 8, 2 | 261082 |
| apfs-newfs-8589934592-plain | 2097152 | 1 + 68 | 69 + 6160 | 6229 + 16 | 6245 + 195 | 64, 1 | 512, 1, 8, 16 | 2090702 |
| apfs-newfs-107374182400-plain | 26214400 | 1 + 280 | 281 + 27672 | 27953 + 16 | 27969 + 2421 | 800, 7 | 512, 3, 8, 100 | 26184000 |

`L` and `I` are the main and internal-pool free-queue node limits, `M` the
minimum block count in ephemeral-information word 0, `F` the maximum volume
count. The `hdiutil` capture equals the `newfs` capture of its size in
every container value but the UUID.

Constant in every capture: superblock type `0x80000001`; block size 4096;
incompatible features `0x2`, all other feature words and flags zero;
ephemeral-information word 0 `(M << 32) + 0x40001`, words 1 to 3 zero;
mapping blocks `0x4000000c` with OID equal to their block, flag `0x1`, each
mapping size 4096 and owning volume zero; container object map
`0x4000000b`, flags `0x1`, tree and snapshot-tree types `0x40000002`, no
snapshot tree; its root leaf `0x40000002` subtype `0xb`, node flags `0x7`,
a 448-byte table of contents at offset 0, tree flags `0x12`, keys and
values of 16 bytes; space manager flags `0x1`, version 1, structure size
2520, 32768 blocks per chunk, 126 chunks per chunk-information block, 507
per address block, no address block, pool ring of 16 blocks with a
one-block bitmap and transaction multiplier 16, reserves zero, data zone
zero, arrays at 2520, 2528, 2536 and 2568, the tier-2 address array after
the main one; free-queue nodes `0x80000002` subtype 9, node flags `0x7`, a
576-byte table of contents, tree flags `0xe`, keys of 16 and values of 8
bytes; reaper `0x80000011` idle, head and tail zero, flags `0x1`, next reap
ID 1, completed reap ID 0, state buffer size 3984 and zero. Every checksum
the dumper verified matches. A chunk with no allocated block has bitmap
address zero and a free count equal to its block count; the bits past `N`
in the last chunk's bitmap are clear.

**Formulas.** With `chunks = ceil(N / 32768)`, `C = ceil(chunks / 126)`,
`q(x, k) = ceil(x / k)` with 2 raised to 3:

* `F = min(100, ceil(N / 131072))`, [A1]'s one volume per 512 MiB.
* `P = 3 * (chunks + C)`, the pool's block count; the pool ring, 16 blocks,
  lies right after the data area and the pool right after the ring.
* `I = q(P, 1136)`; `L = q(N, 4544)` below 2^18 blocks, `q(N, 2272)` from
  2^18 and 512 from 2^20.
* `M = L` up to 32768 blocks (128 MiB) and 8 above, as [A1] states.
* `E = L + I + 3 + 8 * M * F`; the data area is `4 * E` blocks from
  `1 + D`; the descriptor area `D = 4 * (ceil(E / 101) + 1)` blocks from
  block 1.
* Counter 0 is `20 + C` in the newest ring superblock and `23 + C` in
  block 0 (in the XID-1 ring superblock `7 + C`, read from block 2 of each image,
since the dump decodes only the newest checkpoint); every other counter is zero.

Every capture and every sweep dump fits all of these, except that the two
containers without a volume, 91 and 92 blocks, hold counter 0 at 8 and 12; the sweep's samples
on both sides of 4544, 9088, 13632, 2^15, 2^17, 2^18 and 2^20 blocks fix the
breakpoints. The sweep holds no address block, so the address-block
threshold, and whether address blocks count in `P`, stay open (E12); no
sample reaches a pool bitmap of more than one block (`P` above 32768).

**Free queues and pool contents.** At XID 2 the internal-pool queue holds
the pool blocks the space manager rewrote: blocks 149 and 150 of
apfs-newfs-33554432-plain (the XID-1 chunk-information and bitmap blocks),
one ghost record at the pool base in the 8 GiB and 100 GiB captures. The
main queue holds two blocks, where apfs-newfs-372736-plain keeps its
container object map and that map's node: right after the pool up to 1 GiB, and at blocks 122880 and 727449 in the 8
GiB and 100 GiB captures (chunks 3 and 22); there the XID-2 objects lie in
chunks 2 and 11.
Queued blocks are allocated in the bitmaps, and the device's free count
equals the bitmap recount. The pool's XID-1 layout is chunk-information
blocks from the base, then bitmap blocks in chunk order
(apfs-newfs-107374182400-plain: seven chunk-information blocks at 27969 to
27975, bitmaps of chunks 0 and 22 at 27976 and 27977). The pool ring at
XID 1 has head 1, tail 15, slot 0 current and free-next `0xffff, 2, 3,
..., 15, 0xffff`; at XID 2 head 2, tail 0, slot 1 current and free-next
`0xffff, 0xffff, 3, ..., 15, 0`.

**Design edits.** The format design's checksum, superblock type word,
optional features, checkpoint candidate rule, unused descriptor slots,
space-manager table and arrays, geometry relations, pool contents, free
chunks, free-queue forms, reaper, node flags and the container object map's
flags take these observations, and its [writer
values](../specs/design-apfs.md#writer-values) take the formulas and the
first form.

### E8 Checkpoint history

**Question.** How the native driver fills and reuses the checkpoint areas
across mounts, what block 0 holds against the descriptor ring, how the free
queues and the container object map change, and what native tools do with
a container whose newest checkpoint is damaged or which is cut short. It
feeds [checkpoint selection](../specs/design-apfs.md#checkpoint-selection),
[truncated containers](../specs/design-apfs.md#truncated-containers) and
[free queues](../specs/design-apfs.md#free-queues).

**Captures.** Outside the repository, under `/Volumes/Optaine/fsimg-apfs/`:

| Path | Content |
| --- | --- |
| `native/apfs-newfs-1073741824-e8-history/` | The base capture: `capture.rb --kind newfs --size 1073741824 --option e8-history`, one case-insensitive volume `FSIMG`, `fsck_apfs -n` clean |
| `e8/cNN.img`, `e8/cNN.json`, `e8/ring-NN.txt` | The container after mount cycle `NN` (01 to 07; `c00.json` and `ring-00.txt` describe the base image), its `apfsdump.rb --json` dump, and a per-slot summary of the descriptor ring |
| `e8/live-05.img`, `e8/live-05.json`, `e8/ring-live-05.txt` | A clone of the backing file taken during cycle 5 while the volume was mounted, after `sync` |
| `e8/derived/NAME.txt`, `NAME.json`, `NAME.mount.txt` | Each damaged copy's `fsck_apfs -n` and `fsapfsinfo` output, its dump, and its read-only mount; the copies themselves are deleted, and `e8/derive.sh` and `e8/zero-many.sh` remake them from `c07.img` and `live-05.img`. The exit status `NAME.txt` prints for `fsapfsinfo` is that of a pipe to `head` |
| `e8/derived/exits.txt`, `NAME.quick.out`, `NAME.quick.txt` | The exit statuses of `fsapfsinfo` on every copy and of `apfs-dump-quick` on two, from a rerun on copies remade the same way, and `apfs-dump-quick`'s output and log |
| `e8/mountpoint.txt` | The refused mount-point attempts of the procedure |
| `e8/cycle.sh`, `ring.rb`, `derive.sh`, `mountro.sh`, `zero-many.sh` | The scratch scripts that ran the cycles, summarized the ring, and made and checked the copies |

**Procedure.** A working copy of the base image is attached with
`hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage` and its
volume mounted with `diskutil mount`; unprivileged, both `hdiutil
-mountroot` and `diskutil mount -mountPoint` refused a mount point in the
scratch directory. Cycle `n` writes one file of `64 KiB * (3n + 1)` and a
directory holding one small file, deletes cycle `n - 2`'s, runs `sync`,
unmounts, detaches, clones the image with `cp -c` (the scratch volume is
APFS) and dumps the clone. Cycle 1 also renames the volume `E8HIST`. Cycle
5 then writes 20 small files with a `sync` after each, and cycles 6 and 7
write 100 each, to drive the data area round. The descriptor area has
`D = 16` blocks from block 1 and the data area 992 blocks from block 17.

| Cycle | Ring XIDs (superblocks) | Block 0 XID | Selected slot | Descriptor run | Data run | Main queue count / oldest XID | Internal-pool queue count / oldest XID | Container object map block (XID) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| base | 1-2 (2) | 2 | 3 | 2+2 | 2+4 | 2 / 2 | 2 / 2 | 1060 (2) |
| 1 | 1-7 (7) | 7 | 13 | 12+2 | 22+4 | 32 / 2 | 12 / 2 | 1158 (7) |
| 2 | 4-11 (8) | 11 | 5 | 4+2 | 38+4 | 55 / 2 | 20 / 2 | 1296 (11) |
| 3 | 8-15 (8) | 15 | 13 | 12+2 | 54+4 | 145 / 2 | 24 / 4 | 1486 (15) |
| 4 | 12-19 (8) | 19 | 5 | 4+2 | 70+4 | 286 / 2 | 24 / 8 | 1725 (19) |
| 5 | 36-43 (8) | 43 | 5 | 4+2 | 166+4 | 602 / 11 | 24 / 32 | 1284 (43) |
| 6 | 140-147 (8) | 147 | 5 | 4+2 | 582+4 | 320 / 115 | 24 / 136 | 1321 (147) |
| 7 | 244-251 (8) | 251 | 5 | 4+2 | 6+4 | 321 / 219 | 24 / 240 | 1295 (251) |
| live, in cycle 5 | 33-40 (8) | 19 | 15 | 14+2 | 154+4 | 603 / 8 | 24 / 29 | 2162 (40) |

Runs are written `index+length` within their area, and the selected slot is
the slot of the newest valid superblock in the ring.

**Descriptor ring.**

* Every checkpoint is two descriptor blocks, a mapping block with the
  last-map flag followed by its superblock, so the 16-slot ring holds 8
  checkpoints once full. A cycle commits 4 checkpoints (5 in cycle 1, 24 in
  cycle 5 and 104 in each of cycles 6 and 7), and the 251 checkpoints go
  round the descriptor ring about 31 times. The ring first wraps in cycle 2
  (`ring-02.txt`: XID 9 in slots 0 and 1 beside XIDs 4 to 8 in slots 6 to
  15).
* Slots are reused in order and nothing is cleared: until a slot is
  overwritten it keeps an older checkpoint whose blocks are valid. Before
  the first wrap the unused slots are all zero (`ring-00.txt`,
  `ring-01.txt`).
* No XID appears in two superblocks of a ring in any capture. Every
  superblock's next indices are its own runs' index plus length modulo the
  area, and its next XID is one above its XID.
* Runs start at even slots of an even ring, so no descriptor run straddles
  the ring's end; that case is not observed.

**Data area and ephemeral objects.**

* The selected checkpoint of every capture maps four
  ephemeral objects, in the order space manager (OID 1024), reaper (1025),
  internal-pool free-queue tree (1027) and main free-queue tree (1029), one
  block each, in consecutive data blocks and with the checkpoint's own XID;
  the formatter's first checkpoint (XID 1), still in the ring of the base
  capture and cycle 1, maps only the first two. Every
  checkpoint relocates all four, so the data run advances by 4 per
  checkpoint.
* The data run wraps: XID 249's run is `990+4` and its mapping places the
  space manager at data block 990, the reaper at 991 and the two queue trees
  at 0 and 1 (`ring-07.txt`, slot 0). The run straddles the area's end; no
  single object does.
* Older checkpoints stay whole. In `derived/g-oldest-only` the superblocks
  of the seven newest checkpoints are zeroed, and `fsck_apfs -n` checks the
  oldest one in the ring, XID 244, and reports the container OK.

**Block 0.**

* After every unmount block 0 is the newest ring superblock except for its
  checksum and the low byte of `counters[0]` at offset 984
  (`NX_CNTR_OBJ_CKSUM_SET`), which is 3 larger in block 0 in all eight
  captures, the formatter's included (`ring-NN.txt`, `cNN.json`). Block 0
  is therefore checksummed after the ring copy; that it also reaches the
  disk after it is an inference from that counter, not an observation.
* During a mount block 0 is not rewritten: the live clone holds block 0 at
  XID 19, the previous unmount's, while the ring holds XIDs 33 to 40
  (`ring-live-05.txt`). A container whose mount a crash interrupts after the
  mount has committed a checkpoint therefore has a block 0 older than its
  newest checkpoint, and no capture shows a block 0 newer than every
  checkpoint in the ring.

**Free queues and the object map.**

* In both queues with entries, in every capture, the count equals the sum
  of the records' runs and the oldest XID equals the smallest record XID.
  The node limits keep the formatter's values, 116 for the main queue and 1
  for the internal pool's, while the main queue's single leaf holds up to
  159 records (`c07.json`). The tier-2 entry stays all zero. The base
  capture's queues already hold entries, 2 blocks each at XID 2, and its
  block 0 differs from the ring superblock as above. Both differ from the
  [writer values](../specs/design-apfs.md#writer-values) for block 0 and
  the free queues, which E1 owns; whether checkpoint XID 2 is
  `newfs_apfs`'s own or written by the capture's later `hdiutil attach
  -nomount` is not established here.
* Freed blocks are held for a window of transactions: from cycle 5 on, and
  in the live clone, the main queue's oldest XID is the checkpoint's XID
  less 32, and from cycle 3 on the internal pool's is the checkpoint's XID
  less 11. Both windows are wider than the 8 checkpoints the ring holds,
  which explains why the older checkpoints' physical objects are still in
  place.
* The container object map is a new physical object in every cycle, with
  the checkpoint's XID, and keeps one record, the volume superblock, whose
  object XID equals its key's XID in every capture.

**Damaged copies.** Copies of `c07.img` (block 0 equal to the newest
checkpoint, XID 251 in slots 4 and 5, data run `6+4` at blocks 23 to 26)
and of `live-05.img` (block 0 at XID 19, newest checkpoint XID 40 in slots
14 and 15, data run `154+4` at blocks 171 to 174). `fsck_apfs -n` runs on
the attached whole disk; the read-only mount is `diskutil mount readOnly`.

| Copy | Damage | `fsck_apfs -n` | Read-only mount | `fsapfsinfo` | `apfsdump.rb` |
| --- | --- | --- | --- | --- | --- |
| `base-c07` | None | Checks XID 251; OK, exit 0 | Mounts | Exit 0 | Selects XID 251, no problems |
| `a-sb-zeroed` | Newest superblock (block 6) | Checks XID 250, with no message about the lost checkpoint or block 0; OK, exit 0 | Mounts | Exit 0 | Selects XID 250; block 0 newer than the latest checkpoint |
| `b-map-zeroed` | Newest mapping block (block 5) | `cpm: found zeroed-out block`, `checkpoint 5 (xid 251) checkpoint map is invalid`, then checks XID 250; OK, exit 0 | Mounts | Exit 0 | Keeps XID 251 and reports its run broken |
| `c-data-zeroed` | Newest data run (blocks 23 to 26) | `nr: found zeroed-out block`, `checkpoint 5 (xid 251) failed consistency check`, then checks XID 250; OK, exit 0 | Mounts | Exit 0 | Keeps XID 251 and reports every mapped object invalid |
| `g-oldest-only` | The seven newest superblocks (blocks 2, 4, 6, 10, 12, 14, 16) | Checks XID 244; OK, exit 0 | Not run | Exit 0 | Selects XID 244; block 0 newer |
| `live-base` | None | Checks XID 40, with no remark on the older block 0; OK, exit 0 | Mounts; files `b0` to `b19` | Exit 0 | Selects XID 40 |
| `live-a-sb-zeroed` | Newest superblock (block 16) | Checks XID 39; OK, exit 0 | Mounts XID 39's tree: `b19`, present at XID 40, is absent | Exit 0 | Selects XID 39 |
| `live-c-data-zeroed` | Newest data run (blocks 171 to 174) | `checkpoint 15 (xid 40) failed consistency check`, then checks XID 39; OK, exit 0 | Mounts XID 39's tree: `b19` is absent | Exit 0 | Keeps XID 40 and reports the mapped objects invalid |
| `d-trunc-ring` | Cut after block 8, inside the descriptor ring | The attach publishes no volume; `device /dev/rdisk7 failed to open with error: Result too large`, exit 66 | No volume | Exit 1: the read of block 9 fails | Every missing block reported |
| `e-trunc-data` | Cut after block 24, inside the selected data run | As `d-trunc-ring`, exit 66 | No volume | Exit 1: the read of the object map at block 1295 fails | Every missing block reported |
| `f-trunc-past` | Cut after block 1008, past both checkpoint areas | As `d-trunc-ring`, exit 66 | No volume | Exit 1, as `e-trunc-data` | Every missing block reported |

* `fsck_apfs` falls back to the next older checkpoint in all three kinds
  of in-place damage: a zeroed newest superblock, a zeroed newest mapping
  block, and valid descriptor blocks over a zeroed data run. The kernel
  falls back in the two kinds tried on the live copies, a zeroed superblock
  and a zeroed data run, where the mounted tree is visibly the older
  checkpoint's and `diskutil` reports an ordinary mount. On the copies of
  `c07.img` the kernel mounts all three, but the two checkpoints' trees
  cannot be told apart, so which one it mounts is not shown; on
  `a-sb-zeroed` it may have used block 0, a valid copy of XID 251.
  `fsck_apfs` exits 0 and reports the container OK while printing `error:`
  lines about the checkpoint it passed over, so its exit status does not
  detect a damaged newest checkpoint.
* `fsck_apfs` prints nothing about a block 0 one XID ahead of the
  checkpoint it checks, and the kernel mounts that copy.
* macOS opens none of the three cut copies, so it selects no checkpoint
  from them either. All three keep only a short prefix; a container
  missing only a short tail is not tried, though the `ERANGE` failure
  suggests the same refusal.
* `apfs-dump-quick` (`derived/a-sb-zeroed.quick.out`,
  `derived/c-data-zeroed.quick.out`) mounts the newest ring superblock
  whose checksum is valid, noting `Mounting xid different from NXSB at 0`:
  on `a-sb-zeroed` it mounts XID 250 and dumps the volume, exit 0; on
  `c-data-zeroed` it mounts XID 251, stops with `Checksum error in spaceman`
  and `Unable to init container`, exit 255, and does not try XID 250.
* `fsapfsinfo` reads into the descriptor ring, failing at block 9 on
  `d-trunc-ring`, but not the selected data run's last two blocks: on
  `e-trunc-data`, which lacks them, it fails only at the object map.

**Consequences.** [Checkpoint selection](../specs/design-apfs.md#checkpoint-selection)
states its duplicate-XID, no-fallback and block-0 rules against this
evidence. The no-fallback and block-0 rules are fsimg policy, stricter than
macOS: fsimg refuses `a-sb-zeroed`, `b-map-zeroed` and `c-data-zeroed`,
which `fsck_apfs` checks at XID 250 and the kernel mounts,
`g-oldest-only`, which `fsck_apfs` checks at XID 244, and
`live-c-data-zeroed`, which `fsck_apfs` and the kernel open at XID 39; it
opens `live-a-sb-zeroed` at XID 39, as both do.
[Truncated containers](../specs/design-apfs.md#truncated-containers)
records the native refusal beside fsimg's own rule.

### E4 Names

**Procedure.** All captures run `capture.rb --kind newfs` on the host
above, once with `--volume name=FSIMG,case-insensitive` (`-ci`) and once
with `--volume name=FSIMG,case-sensitive` (`-cs`). Each populate script
makes one directory `e4` and creates its names in a fixed order with
Ruby's `File.open(path, WRONLY | CREAT | EXCL)` on the raw UTF-8 bytes,
recording each outcome. Of names that are equal under a volume's rules,
the first created is the one stored.

* `e4-names`, 268435456 bytes, 20000 names (list `names.txt`, SHA-256
  `4b285e6a4d6bb04034335ae28dbf6f9f68a930521deffa5e2b84f359f5d7055d`):
  every case-folding source of Unicode 17.0.0 alone and after `x`; every
  case-folding target; every canonical decomposition in its composed and
  decomposed forms, among them the 1002 CJK compatibility ideographs and
  their 902 distinct unified ideographs; 300 Hangul syllables precomposed
  and as conjoining jamo; the characters each Unicode version from 10.0.0
  to 17.0.0 first assigns with a case folding, a decomposition or a
  nonzero combining class, plus 120 others per version; 466 distinct
  supplementary-plane characters; hand-made groups (ASCII, German sharp s,
  Turkish I, Greek sigma, Angstrom, Ohm and Kelvin signs, composed forms,
  foldings not closed under decomposition, Georgian Mtavruli); and random
  names of up to eight of these code points.
* `e4-prefix`, 33554432 bytes: `pirhv`, `p`, `q` and `qodfh`, each long
  name sharing its prefix's hash.
* `e4-u17marks`, 33554432 bytes: each of the 34 combining marks Unicode
  17.0.0 adds after `a`, on both sides of U+0301 (marks of class 220 and
  234) or of U+0323 (marks of class 230).
* `e4-u18`, 33554432 bytes: the 84 names that discriminate Unicode 18.0.0
  from 17.0.0, each new case pair as target then source and each new mark
  of combining class other than 230 on both sides of U+0301.
* `e4-unassigned`, 33554432 bytes: unassigned code points, noncharacters
  and private-use characters (its files are named `*-cn-*`).

Every capture's `fsck_apfs -n` reports the container OK. Each record's
stored bytes, length field and hash come from `apfsdump.json`
(`apfsdump.rb --records --json`) beside each image. The populate scripts,
name lists, per-name outcomes and throwaway analysis scripts are in
`$FSIMG_APFS_NATIVE/e4-inputs/`. The analysis scripts read the two large
captures' `apfsdump.json` copied beside them as `dump-ci.json` and
`dump-cs.json`; `gen-names.rb` ran when the UCD directory held 9.0.0 to
17.0.0; and the archived `populate-common.rb` takes the list name as a
parameter, which the two large captures ran without.

**Captures and vectors.** `apfs-newfs-268435456-e4-names-ci` and `-cs`,
and `apfs-newfs-33554432-e4-` followed by `prefix`, `u17marks`, `u18` and
`unassigned`, each `-ci` and `-cs`.
`$FSIMG_APFS_NATIVE/e4-vectors.txt` holds the 33369 stored names of the
two large captures as `name-hex`, personality (`ci` or `cs`), length field
and hash, tab-separated, and `e4-equivalence.txt` beside it every
creation's outcome with the stored name it matched.
`test/fixtures/apfs/name-hashes.txt` holds 219 vectors covering every
group in both personalities: 211 from `e4-vectors.txt` and the 8 rows of
the prefix captures, whose `p` and `q` on the case-sensitive volume also
appear in `e4-vectors.txt` with the same hashes.

**Outcomes.** No creation in `e4-names` failed with `EILSEQ` or `EINVAL`.
Every stored name is the byte string its successful creation passed, and
every length field is its byte length plus one.

| Capture | Records in `e4` | `EEXIST` | Equal-hash pairs |
| --- | --- | --- | --- |
| `e4-names-ci` (incompatible feature `0x1`) | 15774 | 4226 | 34 |
| `e4-names-cs` (incompatible feature `0x8`) | 17595 | 2405 | 29 |

| Group | Names | `EEXIST`, ci | `EEXIST`, cs |
| --- | --- | --- | --- |
| Case-folding sources | 3133 | 131 | 22 |
| Case-folding targets | 1496 | 1496 | 26 |
| Canonical decompositions | 1315 | 1027 | 1027 |
| CJK compatibility and unified ideographs | 1904 | 1002 | 1002 |
| Hangul | 609 | 305 | 305 |
| ASCII | 15 | 6 | 0 |
| German sharp s | 14 | 11 | 0 |
| Turkish I | 15 | 6 | 1 |
| Greek sigma | 16 | 11 | 0 |
| Angstrom, Ohm and Kelvin signs | 11 | 8 | 5 |
| Composed forms | 20 | 14 | 10 |
| Folding not closed under decomposition | 28 | 18 | 7 |
| Georgian Mtavruli | 8 | 4 | 0 |
| Supplementary planes | 466 | 3 | 0 |
| First assigned in 11.0.0, 12.0.0, 13.0.0, 14.0.0, 16.0.0, 17.0.0 | 1305 | 79, 7, 3, 40, 27, 28 | 0 |
| First assigned in 10.0.0, 12.1.0, 15.0.0, 15.1.0 | 284 | 0 | 0 |
| Random mixed names | 9361 | 0 | 0 |

Selected equivalences on the case-insensitive volume: `ẞ`, `ss`, `SS` and
`Ss` equal `ß`; `i` equals `I`, `i̇` equals `İ`, `ı` and `İ` are distinct
from `I` and from each other, and `İstanbul` and `istanbul` are both
stored; `σ` and `ς` equal `Σ`; `ι` and `Ι` equal U+0345; `ᾼ`, `αι` and
`ΑΙ` equal `ᾳ`; `ff` equals `ﬀ` and `ʼn` equals `ŉ`; `ა` equals `Ა`
(Mtavruli U+1C90); U+212B and `å` equal `Å`. On the case-sensitive volume
only the canonical equivalences hold: `A` U+030A and U+212B equal `Å`,
U+2126 equals `Ω`, U+212A equals `K`, and every jamo spelling and every
CJK compatibility ideograph equals its canonical equivalent.

**The comparison form and the hash.** A throwaway implementation of NFD
and case folding read from each version's `UnicodeData.txt` and
`CaseFolding.txt` tested every candidate against every stored hash and
every outcome of `e4-names` (a creation should fail with `EEXIST` exactly
when an earlier name has the same form):

| Candidate | Unicode | Hash mismatches | Outcome mismatches |
| --- | --- | --- | --- |
| ci: NFD of full folding (`C`, `F`) of NFD | 17.0.0 | 0 | 0 |
| ci: the same | 16.0.0 / 15.1.0 / 14.0.0 / 13.0.0 / 12.0.0 / 11.0.0 / 10.0.0 | 431 / 1065 / 1065 / 1653 / 1707 / 1821 / 2934 | 56 / 130 / 130 / 210 / 217 / 231 / 390 |
| ci: full folding of NFD, without the outer NFD | 17.0.0 | 0 | 0 |
| ci: simple folding (`C`, `S`) | 17.0.0 | 250 | 16 |
| ci: full folding with Turkic `T` entries | 17.0.0 | 252 | 24 |
| ci: NFD of full folding of the name, without the inner NFD | 17.0.0 | 3 | 1 |
| cs: NFD | 17.0.0, 16.0.0 | 0 | 0 |
| cs: NFD | 15.1.0 / 13.0.0 / 12.1.0 | 264 / 264 / 279 | 20 / 20 / 21 |

The hash that reproduces every record is CRC-32C, reflected polynomial
`0x82F63B78`, over the form's code points as little-endian 32-bit units,
from initial value `0xFFFFFFFF`, with no final complement and no
terminating NUL, masked to 22 bits. The other seven combinations of
initial value 0, final complement and NUL fail on the fresh volume's
`root` (2989177) and `private-dir` (2828707).

Between 16.0.0 and 17.0.0 the `e4-names` set discriminates only through
case foldings: 28 code points, such as U+A7CE, U+A7D2, U+A7D4 and U+16EA0
onward (Beria Erfe). `e4-u17marks` adds the canonical ordering of 17.0.0's
new marks: in both personalities each second spelling fails with `EEXIST`,
and every stored hash matches the 17.0.0 form, while 16.0.0 tables
mispredict all 34 outcomes and 30 of the 34 hashes (the other four names
are already in canonical order). Between 15.1.0 and 16.0.0, 47 code points
discriminate, 27 by new foldings (such as U+1C89, U+A7CB, U+10D50) and 20
by new decompositions (such as U+105C9, U+11383). No code point of the set
that an older version assigns changes its folding or decomposition between
the versions. The outer NFD of the case-insensitive form changes no single
code point's form at 17.0.0, and no name of the set tells the two forms
apart; the inner NFD matters for U+0345 before another mark. At 17.0.0 the
largest expansion of one code point's case-insensitive form is three times
its UTF-8 length (U+1D1C0, among others). Ruby 4.0.7's own
`unicode_normalize(:nfd)` misorders Tibetan vowel signs (U+0F73, U+0F75,
U+0F81) after marks such as U+0323 and mispredicts seven stored hashes, so
it is no oracle for these vectors.

**Unicode 18.0.0 and unassigned code points.** In `e4-u18-ci` and `-cs`
every name holding a character Unicode 18.0.0 adds (79 of 84) fails with
`EILSEQ`. The five that succeed hold only 17.0.0 characters (the targets
U+0277, U+027C, U+AB4B, U+AB4C and `ss`), and their stored hashes match the
17.0.0 forms. In `e4-unassigned-ci` and `-cs`, U+0378, `a` U+0378, U+E0000
and the noncharacters U+FFFF, U+FDD0, U+2FFFF and U+10FFFE fail with
`EILSEQ`, while the private-use U+E000 and U+F0000 and the assigned U+2E30
succeed, in both personalities.

**Order and collisions.** In both large captures the directory's records
sort by the 22-bit hash, and the sort by the whole 32-bit word with the
length below it fails. All 34 and 29 equal-hash pairs (none larger) hold
distinct names with distinct forms, each created successfully and ordered
by their bytes as unsigned bytes. The tie order differs from the order of
the forms in 8 and 3 of the pairs and from length-then-bytes in 17 and 6,
so neither of those is the rule. In the prefix captures `p` precedes
`pirhv` and `q` precedes `qodfh` (hashes 4148841 and 3859665) whichever was
created first. The pair counts match a uniform 22-bit hash (about 30 and
37 expected).

### E2 Personalities

Four containers of 64 MiB, one volume each, made with
`FSIMG_APFS_NATIVE=/Volumes/Optaine/fsimg-apfs/native`:

```sh
capture.rb --kind newfs   --size 67108864 --option e2-ci --volume name=E2ci,case-insensitive
capture.rb --kind newfs   --size 67108864 --option e2-cs --volume name=E2cs,case-sensitive
capture.rb --kind hdiutil --size 67108864 --option e2-ci --volume name=E2ci,case-insensitive
capture.rb --kind hdiutil --size 67108864 --option e2-cs --volume name=E2cs,case-sensitive
```

Captures `apfs-newfs-67108864-e2-ci`, `apfs-newfs-67108864-e2-cs`,
`apfs-hdiutil-67108864-e2-ci` and `apfs-hdiutil-67108864-e2-cs`, each with
`apfsdump.txt` and `apfsdump.json` (`apfsdump.rb --records`); `fsck_apfs -n`
passes on all four. The JSON dumps were compared field by field, leaving
out checksums, UUIDs, times and the volume name:

| Comparison | Fields that differ |
| --- | --- |
| `newfs_apfs` against `hdiutil`, case-insensitive | None |
| `newfs_apfs` against `hdiutil`, case-sensitive | None |
| Case-insensitive against case-sensitive | The volume's incompatible features alone: `0x1` against `0x8` |

| Volume field | Value on all four |
| --- | --- |
| Incompatible features | `0x1` case-insensitive; `0x8` normalization-insensitive for the case-sensitive personality |
| Optional features | `0x2` hard-link map records |
| Read-only compatible features | 0 |
| Flags | `0x1` unencrypted |
| Role | 0 |
| Formatting record | `newfs_apfs (2811.160.7.0.4)`, last XID 2; `hdiutil create` runs the same formatter |
| Modification records | All empty |
| Directory-record keys | Hashed in both personalities; the root and private-directory records hash alike in both |
| Next object identifier, next document ID, allocated blocks | 16, 3, 5 |
| Metadata crypto | Version 5.0, class 6, key OS version 424083539, revision 1 |

Neither tool on macOS 26.6.2 offers a volume that is neither
case-insensitive nor normalization-insensitive, so no capture holds plain directory-record keys.

### E3 Population

One volume of each personality in a 256 MiB container, populated by
`/Volumes/Optaine/fsimg-apfs/e3/e3-populate` (a Ruby script; a copy and its
SHA-256 sit in each capture, as `e3-populate` and in `manifest.json`):

```sh
E3_LOG=... E3_MANIFEST=... capture.rb --kind newfs --size 268435456 \
    --option e3-ci --volume name=E3ci,case-insensitive --populate e3-populate
# and --option e3-cs --volume name=E3cs,case-sensitive
```

| Directory | Entries the script creates |
| --- | --- |
| `names` | Files named by 1 byte, 255 ASCII bytes, 85 and 255 copies of U+4E00 (255 and 765 bytes); 255 copies of U+1F600 is refused with `ENAMETOOLONG` |
| `sizes` | Files of 0, 1, 4095, 4096, 4097, 65536 and 1048577 bytes of seeded pseudo-random content; `big`, truncated to `2^32 + 4096` bytes, then `end-of-big\n` written at that offset (`2^32 + 4107` bytes) |
| `nest` | 32 nested directories `d01` to `d32` and a file at the bottom |
| `wide` | 5000 empty files `f00000` to `f04999` |
| `links` | Symbolic links to `../sizes/s1`, `/etc/hosts`, a missing path, `../nest`, `x`, and a 255-byte target |
| `special` | A FIFO (`mkfifo`) and a socket (`bind(2)`); no device: `sudo -n` needs a password, and `mknod` needs root |
| `modes` | Files of mode 04755, 02755, 06755, 0000, 0777 and 0600; directories of 01777 and 02775; a file given group 12 |
| `flags` | Files with `chflags hidden`, `nodump` and `uchg` |
| `times` | Empty files with atime and mtime set by `utimes`: nanosecond fields, before 1970, 2038, 2200; a file whose creation time `SetFile -d` sets to 1990-01-02 03:04:05 host local time |
| `emptydir` | An empty directory |

Captures `apfs-newfs-268435456-e3-ci` and `apfs-newfs-268435456-e3-cs`:
`fsck_apfs -n` passes on both, and each holds `image.img`, the dumps,
`populate.log` (what the script recorded), `native-manifest.json`, and
copies of `e3-populate` and `manifest.rb`. The two differ only in the
incompatible feature and in the `.fseventsd` log files the system writes,
one of which has two extents in `-cs` (29 file extents, 10238 keys, 570
allocated blocks there); observations hold for both.

**Ownership.** `hdiutil attach` mounts the volume with ownership ignored,
so every inode created on it, `.fseventsd` included, is stored with owner
and group 99, and the group change to 12 is not stored. `stat(2)` reports
the mounting user's IDs instead, so a test compares owners against the
dump, not the manifest. Changing the owner needs root, which the run did
not have.

| Entry | Directory record flags | Inode |
| --- | --- | --- |
| Regular file | 8 | Mode `0100644` under umask 022; internal flags `0x8000`; link count 1; name field; a data-stream field when the size is nonzero |
| Directory | 4 | Mode `040755`; internal flags `0x8000`; child count the records it owns; name field; no data stream |
| Symbolic link | 10 | Mode `0120755` under umask 022; internal flags `0x8000`; name field; target in an inline `com.apple.fs.symlink` attribute, flags `0x6` (embedded, filesystem-owned), length the target plus its NUL |
| FIFO | 1 | Mode `010644`; protection class 4; name field only |
| Socket | 12 | Mode `0140755`; protection class 4; name field only |

| Field | Observation |
| --- | --- |
| Inode numbers | Assigned in creation order from 16; the system's `.fseventsd` directory takes 16 and its first file 17. No directory record names an inode below 16 other than the root and private directory. The private directory stays empty, and no inode is unnamed |
| Internal flags | `0x8000` (no resource fork) on every inode; `0x8200` (also sparse) on `big`; `0x8040` (also security attribute) on `.fseventsd`, which holds a 68-byte `com.apple.system.Security` attribute |
| Protection class | 4 on the FIFO and the socket, 0 elsewhere |
| Write generation | Directories 1 plus the child count; files 1 when empty, 2 after one write, 3 for `big`; symbolic links, FIFO and socket 1 |
| BSD flags | `0x8000` hidden, `0x1` nodump, `0x2` uchg; zero elsewhere |
| Modes | The permission bits as set, setuid, setgid and sticky included: `0104755`, `0102755`, `0106755`, `0100000`, `0100777`, `0100600`, `041777`, `042775` |
| Name field | Type 4, flags `0x2`, on every inode, directories included; data the name and its NUL |
| Data-stream field | Type 8, flags `0x20`, size 40, on every file with content and on no empty file. Allocated bytes the size rounded up to 4096; crypto ID 0; total bytes written the bytes written (11 for `big`); total bytes read the bytes read since creation (the manifest read every file) |
| Sparse file `big` | Extents `[0, 2^32 + 4096)` with physical block 0 and one mapped block after it; allocated bytes `2^32 + 8192`, the size rounded up; sparse-bytes field type 13, flags `0x28`, size 8, value `2^32 + 4096` |
| File extents | One per nonempty file without a hole, flags 0, crypto ID 0, length the size rounded up to 4096 (1052672 for 1048577 bytes) |
| References | One data-stream ID record per file with content, count 1; one extent-reference record per mapped extent, kind 1 (new), length in blocks, owner the inode number, count 1 |
| Directory records | No extended fields on any record; hashed keys whose length counts the NUL (766 for the 765-byte name) |
| Times | Stored as given, nanoseconds intact. A time before 1970 is stored as two's complement (mtime `-31535999999999999` as `18415208073709551617`). Setting an mtime earlier than the creation time also lowers the creation time, compared unsigned: `1600000000123456789` lowers it, a negative mtime does not. `SetFile -d` sets the creation time (`631278245000000000`) |
| Volume counters | Files 5032, directories 45 (the root and private directory not counted, `.fseventsd` counted), symbolic links 6, other 2 (FIFO and socket), next object ID 5101, next document ID 3 |
| Allocated blocks | 571 = object map 1 + object-map tree 4 + filesystem tree 258 + extent-reference tree 1 + snapshot tree 1 + file data 306; total blocks allocated 306, the file data alone; total freed 0. An empty E2 volume: 5, total 0 |

**Filesystem tree shape** (`apfs-newfs-268435456-e3-ci`; the `-cs` tree has
257 nodes, a root of 2 keys over two index nodes of 92 and 162):

| Level | Nodes | Keys per node | Table of contents | Free space |
| --- | --- | --- | --- | --- |
| 2 (root) | 1 | 3 | 8 entries (64 bytes) | 3861 bytes |
| 1 | 3 | 2, 92, 160 | 8, 104 and 168 entries | 3920, 210 and 63 bytes, and key and value free lists of up to 297 and 256 bytes |
| 0 (leaves) | 254 | 1 to 88, mean 40 | A multiple of 8 entries: the key count rounded up in 253 leaves, 8 more in one; index nodes up to 8 more | Under 500 bytes in 212 leaves, mean use 3415 of 4040 bytes |

The footer: flags `0x42` (sequential insert, unaligned), node size 4096,
fixed key and value sizes 0, key count 10237 (the leaf records), node count
258, longest key 778 (the 765-byte name's record: 8 + 4 + 766). The longest
value is 924 although the largest live value is 912: the footer can state
more than any live record holds. The wide directory's 5000 records lie
under its inode in hash order across consecutive leaves, each file's inode
record under its own number, and freed key and value space in index nodes
stays on their free lists rather than being compacted.

**Native manifest.** `native-manifest.json` is written by
`/Volumes/Optaine/fsimg-apfs/e3/manifest.rb MOUNT OUT` while the volume is
mounted, as the populate script's last step: a JSON array, sorted by path,
of one object per entry with `path` (relative to the root, `.` for the
root), `kind` (`file`, `directory`, `symlink`, `fifo`, `socket`,
`char_device`, `block_device`), `size` (`st_size`), `mode` (`st_mode &
07777`), `uid`, `gid`, `ino`, `nlink`, `flags` (`st_flags`), `mtime_ns`,
`ctime_ns`, `atime_ns`, `btime_ns` (signed nanoseconds since the epoch),
`sha256` (files only) and `target` (symbolic links only). Contents are
hashed before any entry is stat'ed, so the recorded access times include
the manifest's own reads; a file without read permission is made readable
for the read and restored, which changes its ctime before the stat. Checked
against the dumps, every entry outside `.fseventsd` agrees on inode number,
mode, BSD flags, size and all four times decoded as signed values. Owners
differ as the ownership paragraph states, and `.fseventsd` changes at
unmount, so tests leave that subtree out.

### E14 Limits

`/Volumes/Optaine/fsimg-apfs/e3/e14probe.c`, a C program calling `openat`,
`symlinkat`, `utimensat`, `fchownat` and `mknodat` directly so that no tool
rewrites a name, run by `e14-populate` on one volume of each personality in
a 64 MiB container, followed by the manifest:

```sh
E14_LOG=... E14_MANIFEST=... capture.rb --kind newfs --size 67108864 \
    --option e14-ci --volume name=E14ci,case-insensitive --populate e14-populate
# and --option e14-cs --volume name=E14cs,case-sensitive
```

Captures `apfs-newfs-67108864-e14-ci` and `apfs-newfs-67108864-e14-cs`
(`probe.log` holds the probe output, identical on both apart from creation
times); `fsck_apfs -n` passes on both. Each name limit is a binary search
for the longest accepted repetition of one character; the longest is kept
in the image.

| Name probe | Result |
| --- | --- |
| 255 and 256 copies of `n` (1 byte) | Accepted; `ENAMETOOLONG` |
| 255 and 256 copies of U+0416 (2 bytes) | Accepted (510 bytes); `ENAMETOOLONG` |
| 255 and 256 copies of U+4E00 (3 bytes) | Accepted (765 bytes); `ENAMETOOLONG` |
| 127 and 128 copies of U+1F600 (4 bytes, 2 UTF-16 units) | Accepted (508 bytes); `ENAMETOOLONG` |
| 253 ASCII + U+1F600: 254 characters, 255 UTF-16 units | Accepted |
| 254 ASCII + U+1F600: 255 characters, 256 UTF-16 units | `ENAMETOOLONG` |
| 170 U+4E00 + 85 ASCII: 255 characters, 595 bytes | Accepted |
| `ff`; `61 80 62`; overlong `c0 af`; surrogate `ed a0 80`; `f4 90 80 80` above U+10FFFF; truncated `e4 b8` | `EILSEQ` each |
| U+FFFF (`ef bf bf`), valid UTF-8 of a noncharacter | `EILSEQ` |
| `Icon\r` | Accepted, stored as given |

The limit is 255 UTF-16 code units, whatever the byte length; the longest
name in bytes is 765. No probe character has a decomposition, so the runs
do not tell whether macOS counts the name as given or its NFD. The case-insensitive and the normalization-insensitive
volume answer alike.

| Other probe | Result |
| --- | --- |
| Symbolic-link targets of 1 and 1023 bytes | Created; read back whole; 1023 stored inline with its NUL (1024 bytes) |
| Targets of 1024 and 1025 bytes | `ENAMETOOLONG` |
| `chown` to 0:0 | `EPERM` |
| `chgrp` to 12 | Succeeds, not stored (ownership ignored) |
| `mknod` of a character device | `EPERM` |

| Time set (atime and mtime) | Stored |
| --- | --- |
| -1 s; -1 s + 999999999 ns; `-2^31` s | Two's complement: `0xffffffffc4653600`, `0xffffffffffffffff`, `0xe2329b0000000000` |
| Year 1, `-2^63` ns, one below it, `-10^11` s | `0x8000000000000000`, the clamp |
| `2^63 - 1` ns | `0x7fffffffffffffff` |
| `2^63` ns, `2^64 - 1` ns, above `2^64` ns, year 3000, year 9999 | `0x7fffffffffffffff`, the clamp |
| 0 | 0, and the creation time lowered to 0 |

`utimensat` succeeds for every value and `stat(2)` returns the clamped one:
macOS keeps times as signed 64-bit nanoseconds from `-2^63` (1677-09-21
00:12:43.145224192 UTC) to `2^63 - 1` (2262-04-11 23:47:16.854775807 UTC).
A creation time is lowered to a new mtime below it only when the mtime is
smaller compared unsigned, so a negative mtime leaves it alone.

### E5 Links and attributes

**Procedure.** Each capture is a 64 MiB `newfs` container with one
case-insensitive volume, made with
`FSIMG_APFS_NATIVE=... E5_OUT=<capture>/native-manifest.ndjson
tools/apfs/capture.rb --kind newfs --size 67108864 --option LABEL
--populate SCRIPT`; `fsck_apfs -n` reports every one OK. The populate
scripts and their helpers live in the experiment's scratch directory and
are not committed; `manifest.json` of each capture records the script's
path and SHA-256. Three helpers: `pattern.rb SIZE TAG` prints SIZE bytes in
which every 4096-byte block starts with `TAG block N` and is filled with a
letter that depends on TAG and N, so every block differs; `setxattr.rb PATH
NAME SIZE TAG` sets one attribute to that pattern with `setxattr(2)` and
`XATTR_NOFOLLOW` (an argument list cannot carry a 1 MiB value for `xattr
-wx`); `manifest.rb MOUNT OUT` writes the native manifest described below.
The links script (`$s` is the scratch script directory):

```sh
for g in two three back backp drop same; do mkdir -p "$m/$g/a" "$m/$g/b" "$m/$g/c"; done
$s/pattern.rb 5000 two > "$m/two/a/f";    ln "$m/two/a/f" "$m/two/b/f"
$s/pattern.rb 5000 three > "$m/three/a/f"; ln "$m/three/a/f" "$m/three/b/f"; ln "$m/three/a/f" "$m/three/c/f"
$s/pattern.rb 5000 back > "$m/back/a/f";  ln "$m/back/a/f" "$m/back/b/f";  ln "$m/back/a/f" "$m/back/c/f"
rm "$m/back/b/f" "$m/back/c/f"
$s/pattern.rb 5000 backp > "$m/backp/a/f"; ln "$m/backp/a/f" "$m/backp/b/f"; ln "$m/backp/a/f" "$m/backp/c/f"
rm "$m/backp/a/f" "$m/backp/b/f"
$s/pattern.rb 5000 drop > "$m/drop/a/f";  ln "$m/drop/a/f" "$m/drop/b/f";  ln "$m/drop/a/f" "$m/drop/c/f"
rm "$m/drop/a/f"
$s/pattern.rb 5000 same > "$m/same/a/x";  ln "$m/same/a/x" "$m/same/a/y"
sync; $s/manifest.rb "$m" "$E5_OUT"; sync
```

The attribute scripts set one attribute per file so each record's form can
be read from the dump:

```sh
# e5-xattr: user.v of each size on x/sN, user.sN of each size on the
# directory x and on x/all, plus xattr -w user.tool on x/all
for n in 0 1 3803 3804 3805 65536 1048576; do
  : > "$m/x/s$n"; $s/setxattr.rb "$m/x/s$n" user.v $n "v$n"
  $s/setxattr.rb "$m/x" "user.s$n" $n "d$n"
done
: > "$m/x/all"
for n in 0 1 3803 3804 3805 65536 1048576; do $s/setxattr.rb "$m/x/all" "user.s$n" $n "a$n"; done
xattr -w user.tool 'written by xattr -w' "$m/x/all"
long=$(printf 'n%.0s' $(jot 127))
: > "$m/x/long"; $s/setxattr.rb "$m/x/long" "$long" 3804 long
$s/setxattr.rb "$m/x/long" "${long}m" 1 toolong   # fails: File name too long
# e5-xbound: user.v of 16 64 128 256 512 1000 1024 1025 2000 2048 2049
#   3000 3072 3500 3700 3790 3800 3802 bytes, one file each
# e5-xname: names v, user.v and 127 x n with v:3800 v:3801 v:3802
#   user.v:3801 long:3000 long:3700 long:3800 long:3801
# e5-xlong: a 127-byte name with every size 3700 to 3800 and a 63-byte
#   name with every size 3780 to 3801, one file each
```

**Native manifest.** `native-manifest.ndjson` sits next to `image.img` in
each capture. It holds one JSON object per line for every entry of the
volume, the root (`/`) first and then depth-first with names in byte order;
`.fseventsd`, `.Spotlight-V100`, `.Trashes` and `.TemporaryItems` at the
root are skipped. Fields: `path`, `kind` (`file`, `dir`, `symlink`,
`fifo`, `socket`, `chr`, `blk`), `size`, `mode` (octal string), `uid`,
`gid`, `ino`, `nlink`, `atime_ns`, `mtime_ns`, `ctime_ns`, `birthtime_ns`
(nanoseconds since the epoch), `sha256` (of a regular file's data or a
symbolic link's target, otherwise null) and `xattrs`, a list of `{name,
size, sha256}` in `listxattr(2)` order, read without following links. The
populate script writes it last, reading every byte once before taking the
`lstat` values, so the recorded times are those stored in the image. The
reads also raise each file's total-bytes-read counter, so the read totals
in these captures count the manifest pass.

**Captures.** `apfs-newfs-67108864-e5-links`,
`apfs-newfs-67108864-e5-xattr`, `apfs-newfs-67108864-e5-xbound`,
`apfs-newfs-67108864-e5-xname` and `apfs-newfs-67108864-e5-xlong`, read
with `apfsdump.rb --records`.

**Hard links.** Directory inodes 19 to 21 are `two/a` to `two/c`, 23 to 25
`three/*`, 27 to 29 `back/*`, 31 to 33 `backp/*`, 35 to 37 `drop/*` and 39
`same/a`.

| Group | Inode, its parent and name field | Link count | Sibling links (ID: parent, name) | Sibling maps |
| --- | --- | --- | --- | --- |
| two | 42, 19, `f` | 2 | 43: 19 `f`; 44: 20 `f` | 43, 44 to 42 |
| three | 45, 23, `f` | 3 | 46: 23; 47: 24; 48: 25, all `f` | 46, 47, 48 to 45 |
| back (first name kept) | 49, 27, `f` | 1 | 50: 27 `f` | 50 to 49 |
| backp (first two removed) | 53, 33, `f` | 1 | 56: 33 `f` | 56 to 53 |
| drop (first removed) | 57, 36, `f` | 2 | 59: 36; 60: 37, both `f` | 59, 60 to 57 |
| same (one directory) | 61, 39, `x` | 2 | 62: 39 `x`; 63: 39 `y` | 62, 63 to 61 |

* Every directory record naming a linked inode, the first name included,
  carries the sibling-ID extended field (type 1, flags `0x2`, 8 bytes)
  equal to its own sibling link's ID. Records of files never linked carry
  no extended fields.
* Sibling IDs are fresh object IDs: the first `ln` takes two, one for the
  existing name and one for the new (43 and 44 after inode 42).
* Unlinking back to one name keeps the records of the remaining name: the
  `back` and `backp` inodes have link count 1, one sibling link, one map,
  and a directory record that still carries the sibling ID.
* The inode's parent and name field name one of its links. When that name
  is removed they move to a remaining one: `backp` to its only name `c`,
  `drop` to `b`, the remaining name with the smaller sibling ID.
* The private directory is unused: its child count is 0 and no inode has
  it as parent. Linked inodes carry internal flags `0x8000` only.
* The volume has the hard-link map feature `0x2`, and every sibling ID has
  a sibling map naming the inode. Sibling links sort under the inode by
  sibling ID.
* All names of one inode share one data stream with data-stream ID count 1
  and one physical-extent record of count 1; the manifest lists each name
  with the same `ino` and `nlink`.

**Attributes.** Inline records have flags `0x2`; stream records `0x11`
and a 48-byte value.

| Name length (bytes) | Largest inline value | Smallest stream value | Captures |
| --- | --- | --- | --- |
| 1 (`v`) | 3800 | 3801 | e5-xname |
| 6 (`user.v`) | 3800 | 3801 | e5-xbound, e5-xname, e5-xattr |
| 63 | 3800 | 3801 | e5-xlong |
| 127 | 3794 | 3795 | e5-xlong, e5-xname, e5-xattr |

* Values of 0 and 1 bytes are inline (data length 0 and 1), as is every
  tested size from 16 to 3800 with a short name; 3801, 3802, 3803, 3804,
  3805, 65536 and 1048576 bytes are streams. The reference's 3804 is never
  reached: native writes at most 3800 bytes inline.
* The observations fit two limits together: a value is inline when it is
  at most 3800 bytes and the record's key and value, `8 + 2 + name length
  with NUL` and `4 + value length`, total at most 3936 bytes. Only the
  127-byte name reaches the second limit, so the 3936 rests on one name
  length.
* A 127-byte name is stored (name length 128 with its NUL); a 128-byte name
  is refused by `setxattr(2)` with `ENAMETOOLONG`.
* Every stream record of a user attribute (29 of 29 across the captures;
  more in e6-xclone) has flags `0x11`: the
  stream flag and `0x10`, which the reference does not define. No inline
  record has `0x10`.
* A stream value is a fresh object ID and a data-stream field: the size,
  allocated bytes the size rounded up to 4096, crypto ID 0, bytes written
  and read 0. The stream's file extents are filed under the stream ID, no
  data-stream ID record exists for it, and its physical-extent record has
  kind new, count 1 and the stream ID as owner (the reference names the
  attribute record's ID).
* Attribute records of one inode sort by name bytes, a proper prefix first:
  `user.s0`, `user.s1`, `user.s1048576`, `user.s3803`, ... , `user.s65536`,
  `user.tool`. A directory's attributes take the same forms, and `xattr -w`
  writes the same inline form as `setxattr(2)`.

### E6 Sparse files and clones

**Procedure.** As in E5, with these populate scripts:

```sh
# e6-sparse
mkdir "$m/s"
$s/pattern.rb 4096 start | dd of="$m/s/start" bs=4096 seek=255 2>/dev/null
$s/pattern.rb 4096 mid0 > "$m/s/mid"; $s/pattern.rb 4096 mid1 | dd of="$m/s/mid" bs=4096 seek=256 conv=notrunc 2>/dev/null
$s/pattern.rb 4096 end > "$m/s/end"; truncate -s 1m "$m/s/end"
: > "$m/s/all"; truncate -s 1m "$m/s/all"
truncate -s 10m "$m/s/unaligned"; printf hello | dd of="$m/s/unaligned" bs=1 seek=5000000 conv=notrunc 2>/dev/null
dd if=/dev/zero of="$m/s/zeros" bs=4096 count=16 2>/dev/null
sync; $s/manifest.rb "$m" "$E5_OUT"; sync

# e6-clone: every original is 16 blocks
blk() { $s/pattern.rb 4096 "$1" | dd of="$2" bs=4096 seek="$3" conv=notrunc 2>/dev/null; }
for g in a b c d e f g; do mkdir "$m/$g"; $s/pattern.rb 65536 "orig$g" > "$m/$g/orig"; done
sync
cp -c "$m/a/orig" "$m/a/clone"
cp -c "$m/b/orig" "$m/b/c1"; cp -c "$m/b/c1" "$m/b/c2"
cp -c "$m/c/orig" "$m/c/first"; blk newc "$m/c/first" 0
cp -c "$m/d/orig" "$m/d/mid";   blk newd "$m/d/mid" 7
cp -c "$m/e/orig" "$m/e/app";   $s/pattern.rb 4096 newe >> "$m/e/app"
cp -c "$m/f/orig" "$m/f/clone"; blk newf "$m/f/orig" 7
cp -c "$m/g/orig" "$m/g/clone"; rm "$m/g/orig"
sync; $s/manifest.rb "$m" "$E5_OUT"; sync

# e6-xclone: a file with stream attribute user.big (65536 bytes) and inline
# user.small, cloned; a clone whose data is written; a clone whose user.big
# is rewritten; and a compressed file
mkdir "$m/x"
$s/pattern.rb 65536 xorig > "$m/x/orig"
$s/setxattr.rb "$m/x/orig" user.big 65536 big; $s/setxattr.rb "$m/x/orig" user.small 1 small
sync
cp -c "$m/x/orig" "$m/x/clone"
cp -c "$m/x/orig" "$m/x/dataw"; $s/pattern.rb 4096 neww | dd of="$m/x/dataw" bs=4096 seek=3 conv=notrunc 2>/dev/null
cp -c "$m/x/orig" "$m/x/xattrw"; $s/setxattr.rb "$m/x/xattrw" user.big 65536 big2
jot 200000 > "$m/plain"; ditto --hfsCompression "$m/plain" "$m/compressed"; rm "$m/plain"
sync; $s/manifest.rb "$m" "$E5_OUT"; sync
```

**Captures.** `apfs-newfs-67108864-e6-sparse`,
`apfs-newfs-67108864-e6-clone` and `apfs-newfs-67108864-e6-xclone`, each
with its native manifest.

**Sparse files.** File extents are (logical offset, length, physical
block); sizes in bytes.

| File | File extents | Size | Allocated bytes | Sparse bytes | Bytes written |
| --- | --- | --- | --- | --- | --- |
| start | (0, 1044480, 0), (1044480, 4096, 203) | 1048576 | 1048576 | 1044480 | 4096 |
| mid | (0, 4096, 204), (4096, 1044480, 0), (1048576, 4096, 205) | 1052672 | 1052672 | 1044480 | 8192 |
| end | (0, 4096, 206), (4096, 1044480, 0) | 1048576 | 1048576 | 1044480 | 4096 |
| all | (0, 1048576, 0) | 1048576 | 1048576 | 1048576 | 0 |
| unaligned | (0, 4997120, 0), (4997120, 4096, 207), (5001216, 5484544, 0) | 10485760 | 10485760 | 10481664 | 5 |
| zeros | (0, 65536, 208) | 65536 | 65536 | none | 65536 |

* Every hole is a file extent whose physical block is 0, at the start, in
  the middle and at the end; a hole made by extending the size is explicit
  too. The extents tile the size rounded up to 4096 with no gap.
* The allocated bytes count holes: they equal the total length of the
  extents, not the mapped bytes.
* A sparse inode has internal flags `0x8200` (`IS_SPARSE` and
  `NO_RSRC_FORK`) and the sparse-bytes extended field (type 13, flags
  `0x28`, system field and children inherit, 8 bytes) equal to the total
  length of its hole extents. Zeros written with `dd` are stored, with no
  sparse flag or field.
* Holes have no physical-extent record. One record covers each allocated
  run, whatever the file extents: `mid`'s blocks 204 and 205 are one record
  of 2 blocks, though they back logical offsets 0 and 1 MiB.
* Bytes written counts the bytes passed to `write(2)`: 5 for `unaligned`.

**Clones.** Inode, its private ID and internal flags; then each stream's
data-stream ID count and file extents; then the physical-extent records
(first block/length: owner, count). Data-stream ID counts not given are 1;
all records have kind new.

| Group | Inodes | Streams | Physical-extent records |
| --- | --- | --- | --- |
| a: clone | orig 19 and clone 32, private 19, both `0x8410` | 19: count 2, (0, 65536, 203) | 203/16: 19, 1 |
| b: clone of a clone | orig 21, c1 33, c2 34, private 21, all `0x8410` | 21: count 3, (0, 65536, 219) | 219/16: 21, 1 |
| c: clone's first block written | orig 23, private 23, `0x8410`; first 35, private 36, `0x8400` | 23: count 1, (0, 65536, 235); 36: count 1, (0, 4096, 324), (4096, 61440, 236) | 235/1: 23, 1; 236/15: 23, 2; 324/1: 35, 1 |
| d: clone's block 7 written | orig 25, private 25, `0x8410`; mid 37, private 38, `0x8400` | 25: (0, 65536, 251); 38: (0, 28672, 251), (28672, 4096, 325), (32768, 32768, 259) | 251/7: 25, 2; 258/1: 25, 1; 259/8: 25, 2; 325/1: 37, 1 |
| e: block appended to the clone | orig 27, private 27, `0x8410`; app 39, private 40, `0x8400` | 27: (0, 65536, 267); 40: (0, 65536, 267), (65536, 4096, 326) | 267/16: 27, 2; 326/1: 39, 1 |
| f: original's block 7 written | orig 29, private 42, `0x8400`; clone 41, private 29, `0x8410` | 29: (0, 65536, 283); 42: (0, 28672, 283), (28672, 4096, 327), (32768, 32768, 291) | 283/7: 29, 2; 290/1: 29, 1; 291/8: 29, 2; 327/1: 29, 1 |
| g: original removed | clone 43, private 31, `0x8410` | 31: count 1, (0, 65536, 299) | 299/16: 31, 1 |

* `cp -c` makes the clone share the source's data stream: the same private
  ID, and a data-stream ID count equal to the inodes sharing it (2, or 3
  for a clone of a clone). Their one physical-extent record has count 1:
  physical counts count streams, not inodes.
* A write to any sharer, the source included (group f), gives the writing
  inode a new stream under a fresh object ID; its extents repeat the old
  mapping with new blocks for the written or appended range, and the old
  stream's count drops by one. The other inode keeps the old stream.
* The physical-extent records split where the branches diverge: the parts
  both streams still map have count 2, a replaced part only the old stream
  maps keeps count 1, and the new blocks get their own record of count 1.
  One file extent may therefore span several records (stream 23's one
  extent covers 235/1 and 236/15), as one record may cover several
  extents (`mid` in e6-sparse). Each record's count equals the number of
  distinct streams with an extent over its blocks, for all 19 records.
* A record's owner is the inode number of the inode whose write allocated
  the blocks, not its private ID: 324 belongs to inode 35, whose stream is
  36, and 327 to inode 29, whose stream is 42. The owner stays when that
  inode moves to another stream or is removed (299 names removed inode 31,
  whose stream lives on in the clone). The reference names the private ID.
* `WAS_CLONED` (`0x10`) is set on the source and on every clone and
  cleared from an inode a write gives a new stream; it stays on an inode
  whose stream is no longer shared (orig 23, count 1). `WAS_EVER_CLONED`
  (`0x400`) stays on all of them. The reference describes `WAS_CLONED` as
  set on the inode created by cloning.
* A clone's data-stream field starts as a copy of its source's: `first`
  has 69632 bytes written, the source's 65536 plus its own 4096.

**Clones with stream attributes** (e6-xclone). `orig` is inode 19 with
data stream 19 and `user.big` in stream 20 (blocks 219 to 234).

| Inode | Data stream | `user.big` stream and its extent |
| --- | --- | --- |
| orig 19, `0x8410` | 19, count 3: (0, 65536, 203) | 20: (0, 65536, 219) |
| clone 21, `0x8410` | 19 | 22: (0, 65536, 219) |
| dataw 23, `0x8400` | 25, count 1: (0, 12288, 203), (12288, 4096, 242), (16384, 49152, 207) | 24: (0, 65536, 219) |
| xattrw 26, `0x8410` | 19 | 28: (0, 65536, 243) |
| compressed 30 | none; `com.apple.ResourceFork` stream 31: (0, 405504, 574) | |

Physical-extent records: 203/3: 19, 2; 206/1: 19, 1; 207/12: 19, 2;
219/16: 20, 3; 242/1: 23, 1; 243/16: 28, 1; 574/99: 30, 1.

* `cp -c` gives the clone a stream ID of its own for each stream
  attribute, with extents over the source's blocks; the inline attribute is
  copied. No attribute stream has a data-stream ID record, and each has one
  attribute using it.
* Record 219's count 3 counts the attribute streams 20, 22 and 24 that map
  it; rewriting `xattrw`'s attribute replaced its stream with 28 on new
  blocks and left the data stream shared.
* The resource fork of the compressed file has flags `0x1` alone, without
  `0x10`, bytes written 402482, no data-stream ID record, and its record
  names the inode 30 as owner, not the stream 31.

**Volume block counts.** In every capture below the volume's total blocks
allocated equals the blocks of its physical-extent records, shared blocks
once, and its allocated block count adds the volume's metadata blocks: the
nodes of the object-map, filesystem, extent-reference and snapshot trees
and one block more, the object-map object or the volume superblock, which
the counts cannot tell apart. Total blocks freed is 0 in each. The totals
are cumulative, as [A1] defines them: e6-xclone, which removes a
315-block file, has 466 allocated and 315 freed, and the difference, 151,
is the blocks of its physical-extent records; its allocated block count
is 156.

| Capture | Allocated block count | Total allocated | Metadata blocks (filesystem-tree nodes) |
| --- | --- | --- | --- |
| apfs-newfs-33554432-plain (fresh) | 5 | 0 | 5 (1) |
| e5-links | 23 | 15 | 8 (4) |
| e5-xattr | 836 | 829 | 7 (3) |
| e6-sparse | 29 | 24 | 5 (1) |
| e6-clone | 126 | 119 | 7 (3) |

### E7 Compression

**Procedure.** On the host above, with stock tools only: `ditto
--hfsCompression`, a preserving `ditto` copy, and Apple Archive's `aa
extract -afsc ALG -afsc-all`, the one stock tool that names the codec
(`afsctool` is not installed and was not used). The scripts live in the
scratch directory `/Volumes/Optaine/fsimg-apfs/e7/scripts`, outside the
repository. `gen-src.rb` writes 20 deterministic sources: `cN` is N bytes
of numbered text lines, `rN` N bytes from Ruby's `Random.new(seed)`, `zN`
N zero bytes and `m200k` 100000 text bytes then 100000 random bytes, at
the sizes the tables below name. `afsc.aar` is `aa archive` of eleven of
them. The two captures:

```sh
FSIMG_APFS_NATIVE=/Volumes/Optaine/fsimg-apfs/native
capture.rb --kind newfs --size 67108864 --option e7-compression \
    --populate populate-e7.sh
capture.rb --kind newfs --size 33554432 --option e7-decmpfs-split \
    --populate populate-e7-split.sh
```

`populate-e7.sh`, given the mount point `$m`:

```sh
for f in "$src"/*; do                    # the 20 sources
	ditto --hfsCompression "$f" "$m/ditto/$(basename "$f")"
done
for a in zlib lzvn lzfse lzbitmap copy; do
	mkdir "$m/afsc/$a"
	aa extract -i afsc.aar -d "$m/afsc/$a" -afsc "$a" -afsc-all
done
ditto /Applications/Xcode.app/Contents/Applications/FileMerge.app \
    "$m/app/FileMerge.app"
```

`populate-e7-split.sh` runs `aa extract -afsc copy -afsc-all` on 46
random files of 100 to 190 bytes, step 2. Both captures end `fsck_apfs -n`
"appears to be OK" with exit 0 and dump with no problem; `apfsdump.json`
and `apfsdump.txt` (both with `--records`) lie beside each image. While
mounted, the populate script read every compressed file's
`com.apple.decmpfs` and `com.apple.ResourceFork` through
`getxattr(XATTR_SHOWCOMPRESSION)` into `native-decmpfs.jsonl` (header,
framing, chunk table, SHA-256 of the contents) and the contents' SHA-256
into `native-sha256.txt`, both in the e7-compression capture directory.
A later read-only attach decoded every stored framing independently of the
kernel (zlib with Ruby's binding, LZFSE and LZVN with `compression_tool
-decode -a lzfse`, LZVN wrapped in a `bvxn` block) and compared the result
with the contents read through the volume: `native-redecode.txt`, 59 of
59 non-LZBITMAP files equal, every chunk exactly 65536 bytes or the
remainder, no zlib input left over; the 11 LZBITMAP files have no decoder.
The same `getxattr` read surveyed the host's own files, and compared the
captured FileMerge files with their Xcode originals. Probes before the
captures, on the APFS scratch volume itself (`/Volumes/Optaine/fsimg-apfs/e7`
`t2`, `t3`, `t4` and `aa`, same sources and commands), bisected the
`ditto` thresholds below and showed `aa` leaving `r65536`, `r1m` and
`r10m` plain.

**Who writes which type.**

| Producer | Types | Observed rule |
| --- | --- | --- |
| `ditto --hfsCompression` | 7, 8 | Compresses only files over 16384 bytes (16384 stays plain, 16385 is compressed) and leaves incompressible ones (`r65536`, `r1m`, `r10m`) plain |
| `aa extract -afsc zlib`, `lzvn`, `lzfse`, `lzbitmap` | 3/4, 7/8, 11/12, 13/14 | Compresses every file it gains on, small ones included; leaves a wholly incompressible file of 65536 bytes or more plain (`r65536`, `r1m`, `r10m`, probe `aa`) and raw-marks an incompressible small payload (`r100`, `r3000`) or chunk (`m200k`) |
| `aa extract -afsc copy` | 9/10 | Stores every file uncompressed behind the `0xCC` marker |
| Preserving `ditto` of FileMerge.app | 8 | Copies type, chunk table and contents unchanged (4 files, compared with the originals) |

`ditto` stores a type inline only when the uncompressed size is at most
65536 bytes and the whole attribute at most 3802 bytes: `z65536` is inline
(514 bytes), `z65537` goes to the fork although its encoding is smaller,
`c27441` is inline at 3802 bytes and `c27442`, 3803 bytes encoded, goes to
the fork (probe `t4` bisected every size from 27400 to 27450). Under its four
codecs `aa` keeps the same 65536-byte limit (`z65536` inline, `z65537` in
the fork), while `-afsc copy` puts `z65536` in the fork as type 10, its
attribute being too large; its attribute limit is only bracketed, a 3017-byte attribute
inline and a 3817-byte one (`c3800` in the probe `aa/copy`) in the
fork. A copy of a `/System` application fails (`ditto`
cannot recreate `restricted` directories unprivileged), hence FileMerge
from the App Store's Xcode.

In the wild, by `decmpfs` type (`/Library` not surveyed):

| Tree | 7 | 8 | 9 | 14 |
| --- | --- | --- | --- | --- |
| `/System/Applications` | | 10096 | 518 | |
| `/System/Library` | | 96577 | 30198 | 65 |
| `/System/iOSSupport` | | 4703 | 61 | |
| `/System/Volumes/Preboot/Cryptexes` | | | 1844 | 7936 |
| `/usr` | | 15537 | 792 | |
| `/Applications` (Xcode) | 1495 | 22153 | | |

No zlib, LZFSE, type 1, 10 or 13 file occurs in the surveyed trees (home
directories not surveyed). All 31569 type 9 files under
`/System/Applications`, `/System/Library`, `/System/iOSSupport` and `/usr`
(`type9-check.rb`, output in `/Volumes/Optaine/fsimg-apfs/e7/type9-check.txt`)
are 1 to 111 bytes, with a payload of `0xCC` then the contents and no
resource fork.

**Header and inode.** Every one of the 70 compressed files in
e7-compression has the header `66 70 6d 63`, a little-endian u32 type and
a little-endian u64 size equal to the contents' length. Every inode has
BSD flags `0x20`, `INODE_HAS_UNCOMPRESSED_SIZE` (`0x40000`) with
`uncompressed_size` equal to the header's size, no data-stream field, no
file extents and no data-stream ID record of its own. The internal flags
are `0x44000` (`HAS_RSRC_FORK`) for the 35 resource-fork files, `0x48000`
(`NO_RSRC_FORK`) for `aa`'s 32 inline ones, and `0x40000` alone for
`ditto`'s 3 inline ones (`c20000`, `c27441`, `z65536`), which also carry
an empty `com.apple.ResourceFork` stream (size 0, no extents).

**Attribute storage.** `com.apple.decmpfs` is held in its record
(`DATA_EMBEDDED`) up to 127 bytes and in a stream (`DATA_STREAM`, one
extent) from 129 bytes: in e7-decmpfs-split the 117- to 127-byte values are
embedded and the 129- to 207-byte ones streamed (128 was not produced), and
in e7-compression every 16-byte header of a fork type and the 100- and
117-byte inline values are embedded, the 200-byte and larger ones streamed.
Every non-empty `com.apple.ResourceFork` is a stream. No attribute stream
has a data-stream ID record: the 61 such records belong to the
uncompressed files.

**Inline payloads.** The bytes after the header:

| Type | Compressed payload begins | Raw-marked payload (`c100`, `r100`, `r3000`) |
| --- | --- | --- |
| 3 zlib | `78 5e` (a zlib stream) | `0xFF`, then the contents |
| 7 LZVN | the first opcode (`e0`, `68`) | `0x06`, then the contents |
| 9 copy | | `0xCC`, then the contents, on every file |
| 11 LZFSE | `bvx2` or `bvxn` | `0xFF`, then the contents |
| 13 LZBITMAP | `5a` (`ZBM`) | `0xFF`, then the contents |

No LZFSE payload or chunk in the capture begins with an uncompressed
`bvx-` block; blocks after the first were not examined.

**Resource-fork framing.** Type 4 (`afsc/zlib`: `c1m`, `c65537`, `m200k`,
`z65537`), offsets from the fork's start:

| Bytes | Field | Observed |
| --- | --- | --- |
| 0-3 | Data offset, big-endian | 256 |
| 4-7 | Map offset, big-endian | data offset + data length |
| 8-11 | Data length, big-endian | to the payload's end |
| 12-15 | Map length, big-endian | 50 |
| 16-255 | | zero |
| 256-259 | Resource length, big-endian | data length - 4 |
| 260-263 | Chunk count, little-endian | `ceil(size / 65536)` |
| 264- | (offset, length) pairs, little-endian, from byte 260 | first offset `4 + 8 * count`, each chunk where the previous ends, the last ending at the data's end |
| map offset | Resource map, 50 bytes | `00` x 24, `00 1c 00 32 00 00`, `cmpf`, `00 00 00 0a 00 01 ff ff`, `00` x 8; the fork ends here |

Types 8, 10, 12 and 14 (every other fork): count + 1 little-endian u32
words from byte 0; the first is `(count + 1) * 4`, each chunk runs from
its word to the next with no gap, and the last word is the fork's length.

Chunks hold 65536 content bytes, the last the remainder. A chunk that does
not compress is the raw marker and its bytes, 65537 stored bytes for a whole
chunk: `m200k`'s last two chunks, wholly random, are raw under LZVN
(`0x06`), zlib, LZFSE and LZBITMAP (`0xFF`), while its second, half text
and half random, is compressed; the 1-byte last chunk of `c65537` and
`z65537` is the marker and that byte, 2 bytes. Every type 10 chunk is
`0xCC` and its bytes. A compressed LZVN chunk ends with `06 00 00 00 00 00
00 00`, an LZFSE chunk with `bvx$`, and a zlib chunk with its Adler-32,
with no input after it.

**Against HFS Plus.** The framing is the one
`lib/formats/hfsplus/codec.c` and `compress.c` decode: the same header,
resource header, resource table, pairs relative to the data offset plus 4,
offset words, 64 KiB chunks and the relations `fork_open` and
`word_table_open` check (`end + map_length` equals the fork's length,
resource length = data length - 4, count = `ceil(size / 65536)`, table
inside the data or the fork), and the same zlib (`0xFF`) and LZVN (`0x06`)
raw markers. Where the HFS Plus reader differs from what macOS writes:

* an LZFSE payload or chunk raw-marked with `0xFF`: `chunk_decode_lzfse`
  passes it to the decoder, which refuses it, and the
  [HFS Plus design](../specs/design-hfsplus.md#inline-payloads) says LZFSE
  has no raw marker, from `afsctool`'s `bvx-` blocks;
* types 9 and 10, which `hfsplus_compress_check` refuses as unsupported.

The APFS storage around the framing differs from HFS Plus: the attributes
are attribute records or attribute streams rather than an attribute B-tree
value and a resource fork.

**Fuzz seeds.** In `$FSIMG_APFS_NATIVE/e7-chunks`, outside the
repository, from `aa extract -afsc` copies made like the capture's:
`t4-chunk0-c65537` (zlib, 5800 bytes), `t8-chunk0-c65537` (LZVN, 8438),
`t12-chunk0-c65537` (LZFSE `bvx2`, 4079), the inline payloads
`t3-inline-c3000`, `t7-inline-c3000`, `t11-inline-c3000` (LZFSE `bvxn`)
and the raw-marked `t3-`, `t7-`, `t11-inline-r100`.

### E10 Volumes

Containers built with `diskutil apfs addVolume` on a raw attach, by the
script `/Volumes/Optaine/fsimg-apfs/e10/e10-volumes.sh`, a copy of which
sits in each capture directory. For each capture it runs:

```sh
mkfile -n SIZE image.img
hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage image.img
newfs_apfs -v None /dev/diskP            # slot 0, no role
diskutil apfs addVolume diskC APFS NAME -role R -nomount [EXTRA]  # per volume
diskutil apfs list diskC                 # saved as diskutil-list.txt (.plist too)
hdiutil detach /dev/diskP
fsck_apfs -n /dev/diskP                  # after a fresh attach -nomount; fsck.txt
```

`diskC` is the synthesized container disk of the physical store `diskP`,
found in `diskutil apfs list -plist`. `-groupWith` names the volume disk of
an earlier volume. The first two captures hold `commands.txt`,
`diskutil-list.txt`, `diskutil-list.plist`, `fsck.txt` and the dumper's
output; e10-hole, made by hand, holds the commands it ran in
`commands.txt`, `diskutil-list.txt` after the refill, and `fsck_apfs -n`
output, exit statuses and dumps of `hole.img` and `refill.img`:

| Capture | Size | Volumes asked for (diskutil role letter) | Result |
| --- | --- | --- | --- |
| apfs-diskutil-2147483648-e10-roles | 2 GiB | None; Data `D` with `-reserve 100m -quota 500m`; System `S -groupWith` Data; Preboot `B`; Recovery `R`; VM `V` | Four volumes; Recovery and VM refused with `-69493: You can't add any more APFS Volumes to its APFS Container` |
| apfs-diskutil-8589934592-e10-allroles | 8 GiB | None; then `D`, `S -groupWith` Data, `B`, `R`, `V`, `I`, `T`, `E`, `X`, `H`, `C`, `Y`, `BR`, `0 -quota 50m`, and two without `-role` | Fourteen volumes; `X`, `Y` and `BR` refused with `-69624: Unable to add a new APFS Volume to an APFS Container` |
| apfs-diskutil-2147483648-e10-hole | 2 GiB | The e10-roles image; `diskutil apfs deleteVolume` of Data (slot 1), copied as `hole.img`; then `addVolume New -nomount`, copied as `refill.img` | Both `fsck_apfs -n` clean |

Every container passes `fsck_apfs -n`. Container values: maximum volume
count 4 at 2 GiB (524288 blocks) and 16 at 8 GiB (2097152 blocks), the
`min(100, ceil(N * B / 512 MiB))` rule, and `diskutil` enforces it; the
container incompatible features are `0x2` and its flags 0, with or without
a volume group.

Slots and objects. Volumes fill the volume array from slot 0 in creation
order, and each superblock's index equals its slot. The superblock OIDs are
1026 for the formatter's volume and then 1030, 1032, 1034 and so on, two
apart: each added volume takes the next OID for its superblock and the one
after for its root tree (slot 1: superblock 1030, root tree 1031; slot 0's
root tree is 1028). The container object map holds exactly one record per
volume, its superblock OID, and each volume's object map one record, its
root tree; every volume object map has flags 0. In e10-hole, deleting slot
1 zeroes that slot (`fs_oid` 1026, 0, 1032, 1034) and the next volume added
takes the lowest free slot, 1, with a new OID, 1037 (the container's next
OID); no other volume moves.

Roles, as stored and as `diskutil` reports them:

| Letter | Stored role | [A1] name | `diskutil apfs list` | Plist `Roles` |
| --- | --- | --- | --- | --- |
| none, `0` | 0 | `NONE` | No specific role | empty |
| `S` | 1 | `SYSTEM` | System | System |
| `R` | 4 | `RECOVERY` | Recovery | Recovery |
| `V` | 8 | `VM` | VM | VM |
| `B` | 16 | `PREBOOT` | Preboot | Preboot |
| `I` | 32 | `INSTALLER` | Installer | Installer |
| `D` | 64 (1 << 6) | `DATA` | Data | Data |
| `E` | 192 (3 << 6) | `UPDATE` | not listed | Update |
| `H` | 320 (5 << 6) | `HARDWARE` | Hardware | Hardware |
| `T` | 384 (6 << 6) | `BACKUP` | Backup | Backup |
| `C` | 448 (7 << 6) | `RESERVED_7` | Time Machine Sidecar | Sidecar |
| `X`, `Y`, `BR` | refused | | | |

[A1] lists 448 under the heading `RESERVED_7` with the constant
`APFS_VOL_ROLE_SIDECAR`. `X` and `Y` presumably ask for xART and
Enterprise, but `diskutil` documents only the letters and refuses both
with a generic error. The text form of `diskutil apfs list` leaves the Update volume out
entirely (13 of the 14 volumes in e10-allroles), while the plist form and
`fsck_apfs` include it, so a comparison against `diskutil` uses
`diskutil apfs list -plist`. `diskutil`'s volume disk `diskCsK` is slot
`K - 1` in every E10 container, the hole and refill included.

Volume groups. `-groupWith` on the System volume gives both it and Data a
volume-group UUID equal to Data's own volume UUID, and sets optional
feature `0x10` (volume-group inode space) on both: their features are
`0x12`, every other volume's `0x2`. Neither `diskutil apfs list` form shows
the group. Deleting Data (e10-hole) leaves System with role 0, a zero
group UUID and features still `0x12`, and modifies Preboot (its
modification record names `apfs_kext`), which matches `deleteVolume`'s
documented best-effort removal of Preboot and Recovery entries.

Reserve and quota. `-reserve 100m -quota 500m` stores a reserve of 24415
and a quota of 122071 blocks (the byte counts rounded up to whole blocks),
`-quota 50m` a quota of 12208; volumes without them store zeros. The space
manager's reserve block count is the sum of the volumes' reserves (24415)
and its reserve allocation count 5, the reserved volume's allocated blocks.
`diskutil` reports the reserve as capacity consumed (100003840 bytes for
Data, 20480 for each empty volume). Volumes `newfs_apfs` makes record
`newfs_apfs (2811.160.7.0.4)` as their formatter, those `diskutil` adds
`storagekitd (2811.160.7.0.4)`; all are case-insensitive (incompatible
`0x1`) with flags `0x1`.

### E17 Pending reap

A 2 GiB container with a volume Keep (`newfs_apfs`) and a volume Victim
(`diskutil apfs addVolume`, mounted) holding NFILES files of 1 to 16 KiB in
100 directories; Victim is unmounted, the container detached and copied
(`before-delete`), attached again with `-nomount`, and Victim deleted with
`diskutil apfs deleteVolume`. Every copy is `cp -c` of the backing file
while it is attached (an APFS clone, so each copy is one instant of the
file). Each kept copy was checked with `fsck_apfs -n` after its own
`attach -nomount`; `NAME.fsck.txt` holds the output and `fsck-exits.txt`
the exit statuses. Scripts: `/Volumes/Optaine/fsimg-apfs/e10/e17-reap.sh`,
`e17-reap-force.sh` and `e17-forcemid.sh`, copied into their captures.

| Capture | NFILES | Delete and detach | Copies kept |
| --- | --- | --- | --- |
| apfs-diskutil-2147483648-e17-force | 20000 | `deleteVolume` in the foreground, copies as it returns and 1 s later, then `hdiutil detach -force` (e17-reap-force.sh) | before-delete, after-delete-0, after-detach-force |
| apfs-diskutil-2147483648-e17-clean | 100000 | `deleteVolume` in the background, a copy every 0.1 s while it runs (12 copies, `during-000` to `during-011`), copies as it returns and 1 s later, then a clean `hdiutil detach` | before-delete, during-003, during-005, during-007, after-delete-0, after-detach-clean |
| apfs-diskutil-2147483648-e17-forcemid | 100000 | `deleteVolume` in the background, `hdiutil detach -force` 1 s later, copies just before and after the detach | before-delete, before-force, after-detach-forcemid |

No snapshot was deleted: `tmutil localsnapshot` snapshots every volume in
the host's Time Machine set and cannot target a disk image volume, and
`diskutil apfs` lists and deletes snapshots but cannot make one.

States seen in e17-clean. Of the copies not kept, during-000 to 002 had
the before-delete state, 004 that of 003, 006 that of 005, and 008 to 011
and both after-delete copies that of 007, by their dumps:

| Copy | Checkpoint XID | Slot 1 | Container map has 1030 | Reaper flags | Head and tail | List objects | Next reap ID | Completed reap ID | Reap-list entries | Main free queue blocks | Its XIDs | Free blocks | `fsck_apfs -n` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| before-delete | 1075 | 1030 | yes, XID 1075 | `0x1` | 0 | 0 | 1 | 0 | none | 10034 | 1043 to 1075 | 252618 | exit 0 |
| during-003 | 1078 | 0 | yes, XID 1078 | `0x3` | 10324 | 1 | 2 | 0 | 2 | 16388 | 1078 | 295421 | exit 8 |
| during-005 | 1085 | 0 | yes, XID 1085 | `0x3` | 10324 | 1 | 2 | 0 | 2 | 16388 | 1085 | 410121 | exit 8 |
| during-007, after-detach-clean | 1091 | 0 | no | `0x1` | 10324 | 1 | 2 | 1 | none | 13784 | 1091 | 508440 | exit 0 |

* **Reaping.** In during-003 and during-005 the reaper's object fields name
  kind 13 (a volume superblock), OID 1030, XID 1076, size 4096, owner 0 and
  entry flags `0x5` (valid, call). The state buffer starts with [A1]'s
  `apfs_reap_state_t`: last block reaped 52467, then 186964; snapshot XID
  0; phase 2, `APFS_REAP_PHASE_ACTIVE_FS`.
  The one reap-list object, 10324 (the container's next OID before the
  delete), holds two entries in order: kind `0x40000002` (a physical B-tree),
  owner 1030, OID 2069 (Victim's snapshot-metadata tree), XID 1076, flags
  `0x5`; and kind 13, owner 0, OID 1030, XID 1, flags `0xf` (valid,
  reap-ID record, call, completion). The volume's slot is already zero, so
  no volume selection reaches it, but the container object map still maps
  its superblock, rewritten at every reaping transaction.
* **Accounting.** Each reaping transaction queues the blocks it freed in
  the main free queue at its own XID (16388 blocks in during-003 and
  during-005), and earlier transactions' queues drain into the bitmap, so
  the free count climbs from 252618 to 508440. The half-deleted volume's
  trees stay reachable through the container object map and still
  reference blocks the reaper has freed. `fsck_apfs -n` reports the
  reaping copies as `The container /dev/diskN could not be verified
  completely.` after `Space Verification failed.`, with 326 `underallocation
  detected on Main device` ranges of 32766 blocks in during-003 (blocks
  2068 to 35652) and 7598 ranges of 147466 blocks in during-005 (2068 to
  165027), all below the reap state's last block. In during-003 every
  reported block is free in the bitmap and none is in the main free queue:
  the check finds blocks the deleted volume references that the bitmap
  shows free.
* **Finished.** After the reap the reaper is idle in substance but keeps
  its list: flag `0x1`, one reap-list object (head = tail), completed reap
  ID 1 = next reap ID - 1, and the object fields left at kind 13, size
  4096, OID 0, XID 1, entry flags `0xf`, state buffer zero. The list object
  has count 0, first and last `0xffffffff`, free index 1 and maximum 100;
  its entry slots keep stale contents on the free chain. It is an ephemeral
  object mapped by the checkpoint like the space manager. e17-force (XID 67)
  and both e17-forcemid copies (XID 1088) show the same form, and
  `fsck_apfs -n` accepts it.
* **Persistence.** No copy taken after `deleteVolume` returned holds a
  reaper at work: the reap is checkpointed before the command ends, and
  `after-delete-0` equals the final state in both e17-force and e17-clean.
  A forced detach did not interrupt one either. At 1 s (the kept
  e17-forcemid run) and at 2 s into the delete the reap was already
  complete when the detach ran; at 0.5 s the forced detach failed with
  `Resource busy` (exit 16) and the delete then completed. The 0.5 s and
  2 s runs were not kept. Only copies taken while the command ran caught
  the reaper at work.

### E12 Large containers

**Procedure.** `capture.rb --kind newfs --option e12-1t` at 1 TiB
(apfs-newfs-1099511627776-e12-1t). At 128 TiB the same commands ran by
hand (`e12-sweep/manual-capture.sh`), because `capture.rb` hashes the
whole image and 128 TiB of holes takes days on this host:
apfs-newfs-140737488355328-e12-128t holds `commands.txt`, `fsck.txt` and
`apfsdump.json`, but no manifest and no image hash. Both end `fsck_apfs -n`
with "appears to be OK" and exit 0; both dumps report no problem; the
sparse images hold 4.4 MiB and 37 MiB. A sweep (`e12-sweep/sweep.rb`)
formatted 30 more sizes the same way, checked each with `fsck_apfs -n`
(all OK), kept the dump in `$FSIMG_APFS_NATIVE/e12-sweep/nBLOCKS/apfsdump.json`
and deleted the image (`e12-sweep/sweep.log`): 512, 640 and 768 GiB, 2, 3,
4, 8, 16, 32, 50, 64, 100 and 112 TiB, a bisection of the address-block
threshold between 1 and 128 TiB (4.98 to 64.5 TiB), and three sizes at
23.4 TiB around the cap on `I`. Above 128 TiB it formatted and checked,
without a dump, 256 TiB, 512 TiB, 1, 2 and 4 PiB, and two sizes near
2 PiB, with dumps, bisect the address-block threshold at 16384-byte
blocks. The 1 PiB dump comes from one more format of that size by hand,
with the same commands and no `fsck_apfs`.

**Largest size.** `newfs_apfs` refused no size: every size tried, up to
4 PiB (2^52 bytes), formats and passes `fsck_apfs -n`; larger sizes were
not tried. The superblocks read at 256 TiB and 512 TiB (2^37 blocks) have
4096-byte blocks and the one at 1 PiB 16384-byte blocks
(`e13-layers/e13-record.log`, which records those formats too).

**Container values** (XID 2; base + blocks; all areas contiguous; `S` the
space manager's mapped blocks, `A` the address blocks, `I` and `L` the
internal-pool and main free-queue node limits; `M` 8 and `F` 100 in every
row):

| Capture | `N` | Descriptor | Data | Pool ring | Pool | Chunks, `C`, `A` | `S` | `I`, `L` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| apfs-newfs-1099511627776-e12-1t | 268435456 | 1 + 280 | 281 + 27748 | 28029 + 16 | 28045 + 24774 | 8192, 66, 0 | 1 | 22, 512 |
| e12-sweep/n2093285376 (7.8 TiB) | 2093285376 | 1 + 288 | 289 + 28352 | 28641 + 96 | 28737 + 193167 | 63882, 507, 0 | 2 | 171, 512 |
| e12-sweep/n2093318144 | 2093318144 | 1 + 288 | 289 + 28344 | 28633 + 96 | 28729 + 193179 | 63883, 508, 2 | 1 | 171, 512 |
| apfs-newfs-140737488355328-e12-128t | 34359738368 | 1 + 300 | 301 + 29716 | 30017 + 1552 | 31569 + 3170748 | 1048576, 8323, 17 | 2 | 512, 512 |
| e12-sweep/n68719476736-b16384 (1 PiB) | 68719476736 | 1 + 72 | 73 + 26636 | 26709 + 208 | 26917 + 1575951 | 524288, 1029, 0 | 1 | 128, 128 |

The 1 TiB free count is 268382627 and the 128 TiB one 34356536041; both
equal the bitmap recount.

**Formulas.** E1's formulas hold with four extensions, and with them every
4096-byte dump, the two captures and the 30 sweep dumps from 512 GiB to
128 TiB, reproduces the area sizes and bases, `C`, `A`, `P`, the pool
bitmap and ring, the array offsets, `S`, `I`, `L`, `M` and `F`
(`e12-sweep/check.rb`):

* `A = 0` when `C <= 507`, the chunk-information blocks per address block,
  and `ceil(C / 507)` otherwise. Bisected at 7.8 TiB: 507
  chunk-information blocks have no address block and a two-block space
  manager holding 507 addresses; 508 have two address blocks and a
  one-block space manager. The threshold is that count, not whether the
  addresses fit the object. At 16384-byte blocks it is 2043, again the
  chunk-information blocks per address block: `e12-sweep/n136567848960-b16384`
  (2 PiB, 2043 chunk-information blocks) has none and
  `n136567980032-b16384`, one chunk larger, two.
* `P = 3 * (chunks + C + A)`: address blocks count (128 TiB: 3170748, not
  the 3170697 of `3 * (chunks + C)`).
* The pool bitmap is `b = ceil(P / 32768)` blocks and the ring `16 * b`
  slots (transaction multiplier 16). The space manager's arrays are packed
  after the structure: the ring's XID array at 2520 (`8 * b` bytes), the
  bitmap-index array after it (`2 * b`), the free-next array at the next
  multiple of 8 (two bytes per slot), then the main address array (`A`
  entries, or `C` when `A` is zero) and the empty tier-2 array; 128 TiB:
  2520, 3296, 3496, 6600 and 6736. `S` is the smallest whole number of
  blocks holding all of it: two in the sweep from 3 TiB to 7.8 TiB (196 to
  507 chunk-information blocks, no address block) and from 50 TiB up, where
  the ring arrays alone pass 4096 bytes; one otherwise.
* `E = L + I + 2 * S + 1 + 8 * M * F`, E1's constant 3 being `S = 1`
  (`S = 2` at 128 TiB: `E = 7429`, data 29716); `I = min(512, q(P, 1136))`,
  bisected at 23.4 TiB: `n6290472960` (`P` 580494) has 511,
  `n6290505728` (`P` 580497, the first `q` of 512) and `n6302826496`
  (`P` 581634, `q` 513) have 512.
* Block 0's counter 0 is E1's `23 + C`, plus `A + 1` with address blocks,
  in every dump of either block size (35); the other counters are zero.

At 16384-byte blocks the [A1] relations in `B` hold: 131072 blocks per
chunk, 510 chunks per chunk-information block, 2043 per address block, a
pool bitmap of `ceil(P / 131072)` blocks, and a descriptor area of
`4 * (ceil(E / 408) + 1)` blocks, 408 mappings fitting a block; both node
limits are 128, and `E = 128 + 128 + 2 + 1 + 6400 = 6659` fits the data
area.

**Pool contents.** The internal pool holds the address blocks too. In
apfs-newfs-140737488355328-e12-128t each address block precedes the up to
507 chunk-information blocks it names: the pool base 31569, then
chunk-information blocks 0 to 506 at 31570 to 32076, the second address
block at 32077, blocks 507 on from 32078, and so on to chunk-information
block 8322 at 39908; the bitmaps of chunks 0 to 97 follow at 39909 to
40006, then chunk 6576's XID-1 bitmap at 40007. At XID 2 the space manager
added chunk 3288's bitmap at 40010 and rewrote address block 0 (to 40008) and
chunk-information block 26 (to 40009), and the internal-pool queue holds
31569 and 31596, their XID-1 places, as ghost records. Without address
blocks the layout is E1's: apfs-newfs-1099511627776-e12-1t has
chunk-information blocks 0 to 65 at 28045 to 28110 and the bitmaps of
chunks 0, 1 and 74 at 28111 to 28113, with chunk-information block 0's XID-2
rewrite at 28114 and chunk 37's bitmap at 28115. The XID-2 ring state of
the 128 TiB capture is
E1's with `b = 97`: bitmap indexes 97, 1, ..., 96, head 98, tail 0,
free-next `0xffff` in slots 0 to 97 and 0 in slot 1551. Its XID-1 mapping
block, still in the descriptor ring, maps the two-block space manager to
data slots 0 and 1 and the reaper to slot 2; XID 2 fills slots 3 to 7.

**Placement outside the fixed regions.** The volume superblock, the
container object map and its node lie far above the pool and at no fixed
fraction of `N`: blocks 1232081 to 1232083 (chunk 37) at 1 TiB and
107767403 to 107767405 (chunk 3288) at 128 TiB, with the XID-1 copies the
main queue holds at 2431385 (chunk 74) and 215502028 (chunk 6576). The
writer places its run after the fixed regions instead
([layout](../specs/design-apfs.md#layout)).

**Design edits.** The format design's contiguity rule, address-block
threshold, space-manager arrays and mapped size, and pool contents take
these observations, and its writer values take the extended formulas, the
address-block order, the ring and data-ring forms for `S` and `b` above
one, and 128 TiB as the largest container written.

### E13 Layers

**Procedure.** In `$FSIMG_APFS_NATIVE/e13-layers` (`e13-make.sh`,
`e13-make.log`): `hdiutil create -size 64m -fs APFS -volname E13` with
`-layout GPTSPUD` and `-layout NONE`, then `hdiutil convert` of each to
UDZO, UDBZ, ULFO and ULMO. `hdiutil create` takes `-format` only with
`-srcfolder` or `-srcdevice` ("-format requires -srcfolder or
-srcdevice"), so every compressed image comes from `convert`. Each image
was read with `fsimg i -f IMAGE` from a build of `apfs-land` (1a6ebc3c),
in which APFS is not registered. `e13-record.sh` reruns every read with
its exit status, the `-F dmg` reads, the image classes, the refused
`hdiutil` and `diskutil` commands and the whole APM procedure, recreating
the APM images, into `e13-record.log`.

**UDRW is raw.** `hdiutil create` writes a UDRW image as a flat raw disk
of exactly 64 MiB with no `koly` trailer; `hdiutil imageinfo` names its
class `CRawDiskImage`, and converting a UDZO image back to UDRW gives the
same. The DMG layer never sees one: the layers read it as a bare disk.

**Apple Partition Map.** `hdiutil create -layout SPUD -fs APFS` fails,
"create failed - Invalid argument", exit 1, and `diskutil partitionDisk
diskN APM APFS E13 100%` on an attached raw image refuses: "APFS
Containers are only allowed on GPT partition schemes". `diskutil
partitionDisk diskN APM JHFS+ E13 100%` makes a native map (driver
descriptor `ER`, block size 512; entry 1 `Apple_partition_map` at block 1,
63 blocks, named "Apple"; entry 2 `Apple_HFS` at block 64, 130992 blocks;
entry 3 `Apple_Free`, 16 blocks), and `newfs_apfs -v E13` formats entry 2
as a container, exit 0, `fsck_apfs -n` OK, leaving its type `Apple_HFS`
(e13-apm-native.img). e13-apm-typed.img is a copy with the type rewritten
to `Apple_APFS` by hand, and e13-apm-udzo.dmg a UDZO conversion of the
native one.

**What the layers report:**

| Image | Stack | Report | Exit |
| --- | --- | --- | --- |
| e13-gpt-udrw.dmg | `gpt` | GPT, 512-byte sectors; partition 1 `apfs`, 67067904 bytes at 20 KiB, named "disk image", no filesystem; "Volume: none selected: no recognized filesystem in any partition" | 0 |
| e13-gpt-udzo.dmg | `gpt/dmg` | DMG of 9 chunks (zero, zlib), then as above | 0 |
| e13-gpt-udbz.dmg | `gpt/dmg` | DMG of 9 chunks (zero, bzip2), then as above | 0 |
| e13-gpt-ulfo.dmg, e13-none-ulfo.dmg | | "DMG run uses LZFSE compression, which is not supported" | 1 |
| e13-gpt-ulmo.dmg, e13-none-ulmo.dmg | | "the input passes for both a compressed and an uncompressed image; force the compression or a stack"; with `-F dmg`, "DMG run uses LZMA compression, which is not supported" | 1 |
| e13-none-udrw.dmg, e13-none-udzo.dmg, e13-none-udbz.dmg | | "no partition table or filesystem recognized"; e13-none-udzo.dmg with `-F dmg` the same | 1 |
| e13-apm-native.img | `apm` | APM, 512-byte sectors; partition 1 `Apple_partition_map`, 32256 bytes at 512 bytes, named "Apple", no filesystem; partition 2 `Apple_HFS`, 67067904 bytes at 32 KiB, no filesystem; no volume selected | 0 |
| e13-apm-typed.img | `apm` | As above, partition 2 `Apple_APFS` | 0 |
| e13-apm-udzo.dmg | `apm/dmg` | DMG of 5 chunks (zero, zlib), then as e13-apm-native.img | 0 |

Peeling stops at the partition holding the container, which the layers
find and report with its type; only the filesystem step finds nothing. A
ULMO image is ambiguous without a stack because its first chunk is an xz
stream at offset 0 (`fd 37 7a 58 5a 00`), so the xz probe claims the file
beside the DMG trailer; [LZMA chunks in
DMG](todo-hfsplus.md) tracks the change that lets the trailer outrank it.

**Design edits.** The format design's probe section takes the raw UDRW,
the native APM partition typed `Apple_HFS`, and the ULMO ambiguity. The
[partition-type table](../specs/design-layers.md#partition-types) already
names the GPT type `apfs` and gives APFS no APM type, and the DMG
table refuses lzfse and lzma chunks; neither needs a change.

## Evidence questions

Stage 1 fills this section ([plan](apfs-plan.md), stage 1). Each row is a
question of the plan's table; a part another experiment answers stays open
under that experiment.

| Question | Answer | Open |
| --- | --- | --- |
| Container superblock type word; first unreserved OID; volume-count rule | `0x80000001`; 1024; `min(100, ceil(N * B / 512 MiB))` (E1, every capture) | Reaper and free-queue layouts beyond the fresh state (E8, E16) |
| Fletcher-64 on captured blocks | Reproduces every stored checksum (E1) | |
| Checkpoint area sizes and placement by container size; contiguity | The formulas above, with E12's space-manager term; contiguous in every dump, to 128 TiB and a 1 PiB container of 16384-byte blocks (E1, E12) | |
| Mapped ephemeral objects and their XIDs; ring states | Two checkpoints, XIDs 1 and 2, mappings above (E1); after mount cycles every checkpoint relocates four one-block objects (space manager, reaper, both free-queue trees) carrying its XID, the 1 GiB container's 16-slot ring holds 8 two-block checkpoints reused in order and never cleared, and `fsck_apfs` passes over a newest checkpoint with a zeroed superblock, mapping block or data run to the next older one ([E8](#e8-checkpoint-history)) | |
| Descriptor and data ring slots, mapping order, OIDs from 1024, block 0 against the ring superblock | Slots 0 to 3 and data slots 0 to 5; ascending OID; 1024 to 1027, 1029; block 0 differs only in counter 0 (E1) | What holds OID 1028 (E1's volume records) |
| Space manager fields, internal-pool sizing, chunk structures, address-block threshold, bitmaps of free chunks | As above; free chunks have no bitmap (E1); address blocks above 507 chunk-information blocks at 4096-byte blocks and 2043 at 16384 (provisional at other block sizes), counted in the pool and placed before the blocks they name (E12) | |
| Bitmap bits past `N` | Clear (E1) | Whether `fsck_apfs` accepts them set (E15) |
| Fixed regions as functions of `N`; size step | The formulas above, to 128 TiB; one block (E1, E12) | |
| Free queues of a fresh container | XID 1 zero-OID; XID 2 entries as above; node limits `L` and `I` (E1); with entries the oldest XID is the smallest record XID, the count the sum of the runs, and the node limit keeps the formatter's value ([E8](#e8-checkpoint-history)) | |
| Reaper of a fresh container | Idle, reap IDs 1 and 0, state buffer 3984 bytes (E1) | |
| Object-map flags; object XID equal to its key's XID | Container map `0x1`; the volume record's XID equals its object's (E1), in all nine E8 captures too ([E8](#e8-checkpoint-history)) | Volume maps (E1's volume records); more records (E3) |
| Tree flags, node fill and table-of-contents sizing | Container object map and free-queue trees as above (E1) | Volume trees (E1's volume records, E3) |
| DMG, GPT and Apple Partition Map layouts around APFS | GPT partition of type `apfs`; raw UDRW; UDZO and UDBZ peeled; ULFO refused; ULMO ambiguous unless forced, then refused; native APM partition typed `Apple_HFS` (E13) | |
| Writer acceptance: minimum container size | 1024 blocks, the smallest `fsck_apfs` opens (E1) | Acceptance of the writer's output (E15) |
| Name hash conventions (initial value, complement, terminating NUL), full or simple case folding, Unicode version, hashed or plain keys per personality | CRC-32C from `0xFFFFFFFF`, no final complement, no NUL, low 22 bits; full folding without Turkic entries, NFD before and after; Unicode 17.0.0; both `newfs_apfs` personalities hash (E4) | Plain keys (E2) |
| Comparator tie-breaks for hashed records, attributes and snapshot names | Hashed directory records: 22-bit hash, then name bytes as unsigned bytes, a prefix first (E4) | Attributes and snapshot names (E5); plain directory records (E2) |
| Volume features, flags, role, crypto state and formatting record of `newfs_apfs` and `hdiutil` volumes in both personalities (E2 part) | Identical between the two tools; incompatible `0x1` or `0x8`, optional `0x2`, flags `0x1`, role 0, formatter `newfs_apfs (2811.160.7.0.4)` ([E2](#e2-personalities)). Captures `apfs-newfs-67108864-e2-*`, `apfs-hdiutil-67108864-e2-*`. | |
| Hashed or plain keys per personality (E2 part; hash conventions and folding are E4's) | Hashed in both personalities; no native tool formats a plain-key volume. (E2). | |
| Whether the root and private directories count in the volume's object counts (E3 part) | They do not; FIFOs and sockets count as other objects. Captures `apfs-newfs-268435456-e3-ci`, `-cs`. | |
| Directory-statistics, document-ID, purgeable and firmlink records on native directories (E3 part; whether `fsck_apfs` requires them is E15's) | None on any populated directory; the root alone holds the `purgeable-drecs-fixed` attribute. (E3). | |
| Tree flags, node fill and table-of-contents sizing (E3 part) | Filesystem tree `0x42`; tables a multiple of 8 entries; leaves 1 to 88 keys, mostly under 500 bytes free; footer longest value above every live value ([E3](#e3-population)). (E3). | |
| Name length limit and its unit; refusal of invalid UTF-8 | 255 UTF-16 code units (765 bytes at most); invalid UTF-8 and U+FFFF refused with `EILSEQ` ([E14](#e14-limits)). Whether the count is of the NFD is E4's. Captures `apfs-newfs-67108864-e14-ci`, `-cs`. | |
| Symbolic-link target limit and storage, the mode of a new link; FIFO, device and socket encodings | 1023 bytes, inline attribute with its NUL; mode `0755`; FIFO record type 1 and socket 12, each with protection class 4 and no extended field but the name. Devices open: no root, so E16 answers, and the design's default (a device without the number field has no number) applies. (E3, E14). | |
| Name extended field on every inode; reserved inodes named by records; private-directory contents; internal flags and protection class of new inodes | On every inode; none named below 16 but the root and private directory; private directory empty; internal flags `0x8000`, protection class 0, or 4 on FIFOs and sockets. (E3). | |
| Physical-extent records for unshared files; data-stream totals; volume allocated-block accounting (E3 part; clones are E6's) | One reference record per mapped extent, owner the inode; totals written and read count real I/O; allocated blocks the object map, tree nodes and file data, total allocated the file data. (E3). | |
| Native time and owner limits; whether times are signed | Signed 64-bit nanoseconds, clamped to `-2^63` through `2^63 - 1`. Owner limits open: changing owners needs root, and the image mounts with ownership ignored, storing 99; E16 shows native owners; if it cannot answer, the reader reports stored values and the writer's default owner 0 stands. (E3, E14). | |
| Attribute-name order (with E4) | Attribute records of one inode sort by name bytes, a proper prefix first | E5, e5-xattr |
| Sibling links and maps; link counts | Every name of a linked inode has a sibling link and a sibling map, and its directory record carries the sibling ID; the records stay when the names drop back to one; the inode's parent and name field follow a remaining name; the private directory is unused | E5, e5-links |
| Attribute storage, inline limit, stream-backed attributes | Inline up to 3800 bytes, less with long names (3794 with a 127-byte name, 3800 with a 63-byte one); user stream attributes carry flags `0x11`, a resource fork `0x1`; an attribute stream has one user and no data-stream ID record, and a clone gets its own streams over shared blocks; names up to 127 bytes | E5, E6, e5-xattr, e5-xbound, e5-xname, e5-xlong, e6-xclone |
| Hole representation in sparse files; clone sharing and its reference records | Holes are extents of physical block 0, a trailing hole included, counted in the allocated bytes and the sparse-bytes field; clones share one data stream until a write gives the writer a new one; physical-extent records split where branches diverge, count distinct streams and name the allocating inode | E6, e6-sparse, e6-clone |
| Physical-extent records for unshared files; volume allocated-block accounting (with E3) | One record of count 1 and kind new per allocated run, owned by the inode; total blocks allocated counts data blocks, the allocated block count adds the volume's metadata nodes; total freed 0 | E5, E6, all their captures |
| Compression header, inline marker bytes, resource-fork framing per codec, inline size limit, codecs `ditto` and installers choose | The HFS Plus framing field for field; raw markers `0xFF` zlib, LZFSE and LZBITMAP, `0x06` LZVN, `0xCC` on every type 9 payload and type 10 chunk; inline only up to 65536 bytes, and for `ditto` only up to a 3802-byte attribute; `ditto` writes LZVN (7, 8) above 16384 bytes, system files are types 8, 9 and 14, Xcode's 7 and 8 ([E7](#e7-compression)) | Type 1: no producer or specimen on the host; `aa`'s exact attribute limit (between 3017 and 3817 bytes) |
| Blocks of a deleted volume or snapshot not yet reaped: reaper and reap-list state, and how allocation accounts them | While a deleted volume is reaped: its slot is zero, the container object map still maps its superblock, the reaper has flag `0x2` and names the volume, the reap list holds the volume and the tree being destroyed, freed blocks enter the main free queue at the reaping XID while the volume's still-mapped trees reference them, and the state buffer holds `apfs_reap_state_t`; `fsck_apfs -n` cannot verify such a copy completely, reporting referenced blocks the bitmap shows free. After the reap the reaper keeps one empty reap-list object (E17) | A deleted snapshot (neither `tmutil` nor `diskutil` can make a snapshot of a disk image volume) |
| Volume roles and volume groups in multi-volume containers | The roles table of E10, with stored values; System added with `-groupWith` Data gets Data's volume UUID as the group UUID on both, plus optional feature `0x10` on each; slots fill from 0 and a freed slot is reused (E10) | |

The volume rows (features, records of an empty volume, inode fields) wait
for the dumper's volume part and stay open under E1 and E2.
