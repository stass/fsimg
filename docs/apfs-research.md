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

## Evidence questions

Stage 1 fills this section ([plan](apfs-plan.md), stage 1). Each row is a
question of the plan's table; a part another experiment answers stays open
under that experiment.

| Question | Answer | Open |
| --- | --- | --- |
| Container superblock type word; first unreserved OID; volume-count rule | `0x80000001`; 1024; `min(100, ceil(N * B / 512 MiB))` (E1, every capture) | Reaper and free-queue layouts beyond the fresh state (E8, E16) |
| Fletcher-64 on captured blocks | Reproduces every stored checksum (E1) | |
| Checkpoint area sizes and placement by container size; contiguity | The formulas above; contiguous to 384 GiB (E1) | Above 100663296 blocks (E12) |
| Mapped ephemeral objects and their XIDs; ring states | Two checkpoints, XIDs 1 and 2, mappings above (E1); after mount cycles every checkpoint relocates four one-block objects (space manager, reaper, both free-queue trees) carrying its XID, the 1 GiB container's 16-slot ring holds 8 two-block checkpoints reused in order and never cleared, and `fsck_apfs` passes over a newest checkpoint with a zeroed superblock, mapping block or data run to the next older one ([E8](#e8-checkpoint-history)) | |
| Descriptor and data ring slots, mapping order, OIDs from 1024, block 0 against the ring superblock | Slots 0 to 3 and data slots 0 to 5; ascending OID; 1024 to 1027, 1029; block 0 differs only in counter 0 (E1) | What holds OID 1028 (E1's volume records) |
| Space manager fields, internal-pool sizing, chunk structures, address-block threshold, bitmaps of free chunks | As above; free chunks have no bitmap (E1) | Address-block threshold (E12) |
| Bitmap bits past `N` | Clear (E1) | Whether `fsck_apfs` accepts them set (E15) |
| Fixed regions as functions of `N`; size step | The formulas above; one block (E1) | Above 100663296 blocks (E12) |
| Free queues of a fresh container | XID 1 zero-OID; XID 2 entries as above; node limits `L` and `I` (E1); with entries the oldest XID is the smallest record XID, the count the sum of the runs, and the node limit keeps the formatter's value ([E8](#e8-checkpoint-history)) | |
| Reaper of a fresh container | Idle, reap IDs 1 and 0, state buffer 3984 bytes (E1) | |
| Object-map flags; object XID equal to its key's XID | Container map `0x1`; the volume record's XID equals its object's (E1), in all nine E8 captures too ([E8](#e8-checkpoint-history)) | Volume maps (E1's volume records); more records (E3) |
| Tree flags, node fill and table-of-contents sizing | Container object map and free-queue trees as above (E1) | Volume trees (E1's volume records, E3) |
| Writer acceptance: minimum container size | 1024 blocks, the smallest `fsck_apfs` opens (E1) | Acceptance of the writer's output (E15) |

The volume rows (features, records of an empty volume, inode fields) wait
for the dumper's volume part and stay open under E1 and E2.
