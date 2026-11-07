# EROFS research and oracles

Facts about the external tools that check fsimg's EROFS work. All of them are
development oracles: none is linked into, copied into or required by fsimg.

## Sources and licenses

* erofs-utils, from <https://github.com/erofs/erofs-utils> (mirror of
  <https://git.kernel.org/pub/scm/linux/kernel/git/xiang/erofs-utils.git>).
  Its `COPYING` states that most liberofs files in `lib` and `include` use
  GPL-2.0+ OR MIT, and all other files use MIT unless stated otherwise. The
  source tree carries `LICENSES/GPL-2.0` and `LICENSES/MIT`. Per-file SPDX
  tags in `lib`, `include`, `mkfs`, `fsck`, `dump` and `contrib` also include
  GPL-2.0, `BSD-2-Clause OR GPL-2.0+`, `BSD-2-Clause OR GPL-2.0-only` and
  Unlicense on single files. The oracle is built and run only; no source is
  copied.
* The Linux kernel EROFS driver (GPL-2.0) is exercised through a mount only.
* rust-fs-erofs 0.5.1 from crates.io, repository
  <https://github.com/antimatter-studios/rust-fs-erofs>, license MIT per its
  `Cargo.toml`. It is installed as a binary and run only. Its crate source
  ships its own `CLAUDE.md` and `AGENTS.md`; they are not project
  instructions for fsimg.

## Tool identities

### erofs-utils

* Release tag `v1.9.4`, an annotated tag object `20ca6747ae03043895a3120c1ea3c863a573213a`
  that peels to commit `f36cadb5c563995ab3aa8572a60ed6b721b9557d`.
  `build-aux/build-erofs-utils.sh` fetches the tag at depth 1 and refuses a
  commit that differs.
* Build: `sh build-aux/build-erofs-utils.sh NEW-DIR`. It runs `autogen.sh`,
  then `./configure --prefix=NEW-DIR --with-libzstd --without-libcurl
  --without-openssl --without-libxml2 --without-json-c --without-libnl3
  --disable-fuse`, then `make`, and copies `mkfs.erofs`, `fsck.erofs` and
  `dump.erofs` to `NEW-DIR/bin`. The script fails unless `mkfs.erofs -V`
  lists exactly `lz4, lz4hc, lzma, deflate, zstd`, because configure drops a
  codec whose development package is missing without failing. The configure
  output is kept in `NEW-DIR/configure.log`.
* No macOS patch is needed: v1.9.4 builds unmodified on macOS 26 with Apple
  clang and Homebrew `lz4`, `xz` and `zstd` (found through `pkg-config`),
  system zlib, Homebrew `xxhash`, and `autoconf`, `automake`, `libtool`
  (`glibtoolize`) and `pkgconf`.
* macOS oracle path: `~/.local/share/fsimg-oracles/erofs-utils/bin`.
* `mkfs.erofs -V` prints:

  ```
  mkfs.erofs (erofs-utils) 1.9.4-gf36cadb5
  available compressors: lz4, lz4hc, lzma, deflate, zstd
  ```

* Configure results on macOS: `liblz4` yes with `LZ4_compress_destSize` and
  `LZ4_compress_HC_destSize`; `liblzma` yes with `lzma_microlzma_encoder`;
  `zlib` yes; `libzstd >= 1.4.0` yes with `ZSTD_compress2`; `libxxhash` yes;
  `uuid` yes; FUSE, libcurl, openssl, libxml2, json-c and libnl3 off.
  `config.h` defines `LZ4_ENABLED`, `LZ4HC_ENABLED`, `HAVE_LIBLZMA`,
  `HAVE_ZLIB` and `HAVE_LIBZSTD`.
* Dynamic libraries of `mkfs.erofs`: lz4, lzma, zlib, zstd, xxhash, libSystem.
* `fsck.erofs` 1.9.4 exits 0 after a failed superblock checksum: it prints
  `invalid checksum` and `failed to verify superblock checksum` and stops
  with its status still zero (E1). A harness that uses it as an oracle must
  read the message, not the exit status. It validates no configuration
  record, and it reads a chunk past the image's end as zeros where the
  kernel returns an I/O error (E7).
* `mkfs.erofs` 1.9.4 quirks met while building fixtures: `--zD=1` disables
  directory compression and `--zD` or `--zD=0` enables it; a
  `--compress-hints` file with an algorithm column crashes, so no per-file
  codec mix can be produced; `-E force-inode-compact` flattens every time to
  the epoch silently; under `-E 48bit` a compact inode's time is written as
  an unchecked 32-bit value, so times before 1970 or after 2106 wrap.

#### Codec proof

A source tree with a 200000-byte compressible file, a small file and a symlink
was packed with each of `-zlz4`, `-zlz4hc`, `-zlzma`, `-zdeflate` and
`-zzstd` and unpacked with `fsck.erofs --extract=DIR`. Every `mkfs.erofs` and
`fsck.erofs` run exited 0 and `diff -r` of the source and the extraction was
empty. The compressed images were 8192 bytes for lz4, lz4hc, lzma and zstd and
20480 bytes for deflate, against 200000 source bytes.

#### Linux container

The build script runs unchanged in an `ubuntu:24.04` container. It needs the
packages `autoconf automake libtool make pkg-config gcc git ca-certificates
zlib1g-dev liblzma-dev libzstd-dev liblz4-dev uuid-dev`; the CI Linux image
(`build-aux/linux-check/Dockerfile`) installs all of them. Without `uuid-dev`
the script fails in configure (package `uuid` not found); without
`liblz4-dev` it fails with `unexpected codec set: lzma, deflate, zstd`. With
both installed, `mkfs.erofs -V` prints the same two lines as on macOS.

### Capture tools

`ruby tools/erofs/capture.rb NAME SOURCE -- MKFS-OPTION...` builds one fixture
under `$FSIMG_EROFS_FIXTURES/NAME` (never overwriting; it builds in
`NAME.tmp-PID` and renames last) with a fixed build time (`-T`, from
`SOURCE_DATE_EPOCH`, default 0) and UUID (`-U`). It records the image, the
command with `$SOURCE`, `$FIXTURE` and `$EROFS_UTILS_BIN` placeholders, the
tool versions, the SHA-256, the `dump.erofs` superblock, statistics and root
listing, and a manifest of the source tree (modes, owners, nanosecond mtimes,
hard-link groups, extended attributes, content hashes). The manifest is taken
before and after `mkfs.erofs`, and a difference fails the run.

Chunk-based fixtures from `mkfs.erofs` 1.9.4 are not byte-reproducible: the
upper half of the layout word at 0x10 of a chunk-based inode holds
nondeterministic bytes, so two runs on the same source differ there.

`ruby tools/erofs/erofsdump.rb IMAGE` is the independent structure dumper,
written from `specs/design-erofs.md` alone: it prints every superblock field,
checks the superblock checksum, walks the tree from the root and prints inode
headers, attributes, directory entries, chunk entries and every compressed
index record, and reports each broken design rule as a `problem:` line. The
exit status is 0 for a clean image, 1 for a problem or an unreadable image and
2 when the tree is not decoded (the 48-bit layout and the metadata box). It
lags the design on two rules (`TODO.md`): it prints the
epoch for a compact inode's time instead of adding the word at `0x0C`, and
it computes a compact head's block number without the count marker of a
cluster begun in the previous group, so it misreports 3 files of
`e8-lz4-big16k` and 40 of `e9-mixed-lz4-lzma`.

### rust-fs-erofs

See [Rust oracle](#rust-oracle).

## Linux kernel oracle

The Linux check environment is the Docker VM of colima (macOS
Virtualization.Framework, x86_64), Ubuntu 24.04.4 LTS with kernel
`7.0.0-38-generic`. `/boot/config-7.0.0-38-generic` contains:

```
CONFIG_EROFS_FS=m
# CONFIG_EROFS_FS_DEBUG is not set
CONFIG_EROFS_FS_XATTR=y
CONFIG_EROFS_FS_POSIX_ACL=y
CONFIG_EROFS_FS_SECURITY=y
CONFIG_EROFS_FS_BACKED_BY_FILE=y
CONFIG_EROFS_FS_ZIP=y
CONFIG_EROFS_FS_ZIP_LZMA=y
CONFIG_EROFS_FS_ZIP_DEFLATE=y
CONFIG_EROFS_FS_ZIP_ZSTD=y
CONFIG_EROFS_FS_ZIP_ACCEL=y
# CONFIG_EROFS_FS_ONDEMAND is not set
CONFIG_EROFS_FS_PCPU_KTHREAD=y
# CONFIG_EROFS_FS_PCPU_KTHREAD_HIPRI is not set
# CONFIG_EROFS_FS_PAGE_CACHE_SHARE is not set
```

The kernel is newer than 6.17, the release that has the box (metabox) feature
and than 6.15, which has the 48-bit layout. Sub-page block sizes are accepted
by the driver; a `mkfs.erofs -b1024` image and a `-E48bit` image both mounted
and read back correctly. No image with the box feature was built or mounted:
no option for it appears in `mkfs.erofs --help` of 1.9.4. The kernel logs
`EXPERIMENTAL libzstd compressor in use` when a zstd image is mounted.

Mount procedure for a disposable image under the home directory, which the VM
shares with the host at the same path:

```sh
colima ssh -- sudo modprobe erofs
colima ssh -- sudo mkdir -p /mnt/erofs-t
colima ssh -- sudo mount -t erofs -o ro,loop "$HOME/path/image.erofs" /mnt/erofs-t
colima ssh -- sh -c 'cd /mnt/erofs-t && find . | sort && sha256sum $(find . -type f | sort)'
colima ssh -- sudo umount /mnt/erofs-t
```

The image path is the host path, which is identical inside the VM. `mount` reports
`type erofs (ro,relatime,user_xattr,acl,cache_strategy=readaround)`.

Run on an image built with `mkfs.erofs -zlz4` on macOS from a tree with a
nested directory, a 300000-byte compressible file, a 100000-byte random file,
a small file, an empty directory and a symlink: the file list and the three
SHA-256 sums read through the mount equal those of the source tree. The same
tree packed with lz4hc, lzma, deflate and zstd gives the same SHA-256 for the
large file through the mount.

erofs-utils inside the VM is not needed: images are built on the host and
mounted in the VM.

## Rust oracle

`rust-fs-erofs` 0.5.1 (crates.io; description: pure-Rust EROFS reader and
writer, reads mkfs.erofs 1.9 images with LZ4, LZMA, DEFLATE and ZSTD). Its
binary needs the `cli` feature:

```sh
cargo install rust-fs-erofs --version 0.5.1 --features cli --locked \
    --root ~/.local/share/fsimg-oracles/rust-erofs
```

The binary is `~/.local/share/fsimg-oracles/rust-erofs/bin/rust-fs-erofs`;
`-V` prints `rust-fs-erofs (rust-fs-erofs) 0.5.1`. The subcommand `fs` lists,
stats, reads and exports without mounting, with JSON on stdout, and the image
is the first argument:

```sh
rust-fs-erofs fs IMAGE ls /a
rust-fs-erofs fs IMAGE export /a/big.txt OUT
rust-fs-erofs fs IMAGE export --recursive / OUTDIR
```

On the LZ4 image from the kernel test, `ls` listed the directory, the file
export had the SHA-256 of the source file and the recursive export produced
the tree. Other crates found on crates.io (`erofs` 0.0.2, `erofs-rs` 0.3.0,
`liberofs` 0.0.2) were not installed.

## Per-experiment observations

Fixtures live in `~/fsimg-corpus/erofs`, each with its command record,
`dump.erofs` output, SHA-256 and source manifest. The producer is
`mkfs.erofs` 1.9.4-gf36cadb5 unless named; the kernel is the colima VM's
`7.0.0-38-generic` with 4 KiB pages. Hand-edited images recompute the
superblock checksum. An observation is of these tools; a fact read in
`erofs-utils` or Linux source without an observation is marked (source).
The experiments' full notes, scripts and scratch images are kept outside
the repository under `~/.cache/fsimg-erofs/evidence`.

### E1 Geometry

`e1-geom-b512` to `-b4096`, `e1-chk-b512` to `-b4096`, `e1-bt-*`,
`e1-slot-base-*`, `e1-cfg-*`.

* Checksum: CRC-32C seeded with all ones, stored without the final
  inversion, over `[1024, B)` for `B > 1024` and `[1024, 1024 + B)`
  otherwise; the checksum field reads as zero. 46 single-bit flips: every
  flip inside the range is refused by `fsck.erofs` and the kernel, every flip
  at or past its end is accepted, at all four block sizes. The range covers
  the inodes and data that lie in block 0.
* Block size: `mkfs.erofs` refuses `-b8192`; the kernel and the tools refuse
  `blkszbits` 8, 13, 14 and 16. 512 to 4096 are produced and mount.
* Superblock region: 184 extension slots (end at byte 4096) mount at
  `B` 512, 1024 and 4096; 185 are refused by kernel and `fsck.erofs`. The
  kernel reads configuration records placed at and beyond byte 4096 (a
  poisoned record at 4096 fails the mount; a valid 6000-byte record followed
  by one at 10098 is read); `fsck.erofs` validates no record. `mkfs.erofs`
  1.9.4's own records end within the first 1.2 KiB.
* Block count: `blocks * B` equals the image length in every producer image
  of every family, v1.2 and v1.5 included. The kernel and `fsck.erofs` ignore
  the count: 0, 1, 3, 8 and 17 on a 7-block image all mount and read every
  file.
* Build-time word `0x6C`: zero in every image except `-E 48bit`, where it is
  the build time in seconds (the epoch is 0 there).

### E2 Inodes

`e2-default`, `e2-alltime`, `e2-compact`, `e2-extended`, `e2-preserve`,
`e2-ignoremtime`, `e2-uid*`, `e2-vm-*` (Linux build, owners to
4294967294, nanoseconds, times from -100000.25 to year 2255),
`e2-v15-*`, `e2-v12-*` (older producers built in the VM), `e2-count-*`.
3202 inodes scanned.

* Word `0x0C` of a compact inode: zero in every non-48-bit image; the
  file's time minus the epoch under `-E 48bit`. The kernel reads it as
  unsigned seconds added to the epoch in every image, with or without
  48-bit or `MTIME` (hand-edited copies of `e2-alltime`, epoch 1700000000:
  `0xDEADBEEF` gives epoch + 3735928559, with and without `MTIME`; a
  48-bit root with 1234567 reads as 1234567 over its zero epoch). Word `0x1C`
  and the extended inode's `0x06` and `0x30` to `0x3F`: zero in every
  producer inode, ignored by the kernel when set.
* Time without `MTIME`: only v1.2 omits the bit. Its extended inodes hold
  the source's change time with nanoseconds and its compact inodes the
  image build time. The kernel ignores the bit and presents one time as
  access, change and modification time.
* Compact versus extended: extended when owner, group or link count exceeds
  65535, size exceeds 4 GiB, or the time differs from the epoch in seconds
  (non-48-bit); a compact inode loses its nanoseconds. Extended inodes carry
  64-bit signed seconds and nanoseconds exactly.
* Inode count: distinct inodes, a hard link once, plus the packed inode
  under fragments (1.9.4 and 1.5); v1.2 writes 0.

### E3 Data

`e3-b4096-inline`, `-noinline`, `-inline-compact`, `-inline-extended`,
`e3-b512-inline`, `-noinline`, `e3-sparse`, `e3-sparse-noinline`.

* Layout choice: flat-inline when `size mod B != 0` and the 32-byte inode
  plus the tail fit one block, with the inode moved to the next block start
  when needed; otherwise flat with the tail in its own block. A size that is
  a multiple of `B`, zero included, is flat. `-E noinline_data` affects
  regular files only.
* Flat-inline with a nonzero size that is a multiple of `B`: never produced;
  the kernel and `fsck.erofs` refuse it (`inline data across blocks`). A tail
  ending exactly at the block end is read (hand-built `sBm33-tail-fits-exact`,
  a 4064-byte file whose 32-byte inode starts a block); one byte more is
  refused (`sBm33-tail-crosses`).
* Start word: every producer, v1.2 and v1.5 included, writes `0xFFFFFFFF` in
  a flat-inline inode with no full block (21 of the 25 flat-inline inodes of
  `e3-b4096-inline`) and 0 in an empty file; the kernel reads neither (7,
  9999 and `0xFFFFFFFF` all ignored). A flat file holds its start block at
  any nonzero size: the one-byte file of `e3-b4096-noinline` is flat with
  start block 1. A flat-inline inode with a full block and start
  `0xFFFFFFFF` fails to read; a flat 8192-byte file with start `0xFFFFFFFF`
  reads as zeros in the kernel.
* Sparse sources are materialised as zero blocks; no flat hole exists.

### E4 Directories

`e4-dirs-b4096` (22 root entries including invalid UTF-8, a control byte,
names before `.`, 1- and 255-byte names; `many/` with 700 entries in 10
blocks), `e4-exact-b512`, `e4-spare-b512`, `e4-nearfull-b4096`,
`e4-full4096-b4096`, `e4-ext-b4096`.

* Every directory is in strict bytewise order across blocks, `.` and `..`
  included (`.` is the root's eighth entry). Blocks are filled greedily and
  their unused bytes are zero; a name ending at the block end has no NUL; a
  flat final block is zero-filled past `size`.
* Link count 2 + subdirectories on every directory, compact and extended.
  Exception (also source, `lib/inode.c`): a compact directory whose count
  would exceed 65535 is written with link count 1; the kernel reports 1 (a
  66000-subdirectory image, not kept).
* Invalid UTF-8 and control bytes are written, listed and read by name by
  the kernel and extracted by `fsck.erofs` on ext4.

### E5 Kinds

`e5-kinds-b4096`, `e5-kinds-b4096-mtime`, `e5-kinds-b512`: twelve devices
with majors to 4095 and minors to 1048575, a FIFO and a socket.

* Device word: minor's low 8 bits in bits 0 to 7, major in 8 to 19, the
  minor's high bits in 20 to 31, unconditionally; the kernel returns each
  `mknod` value. `0xFFFFFFFF` is device 4095:1048575.
* Devices, FIFOs and sockets: size 0, layout 0, no inline data, no
  attributes; FIFO and socket layout word 0. Entry types 3, 4, 5, 6.

### E6 Attributes

`e6-xattr-default`, `-filter`, `-nofilter`, `-shared0`, `-shared-none`,
`-pfx-plain`, `-pfx-plain-filter`, `-pfx-explicit-plain` (`-b512`),
`-pfx-packed` (`-zlz4 -E fragments`); 26 inodes with `user.`, `trusted.`,
`security.`, both ACL kinds, shared and long-prefix names. The kernel
returns every attribute equal to the source tree in all eight images with
attributes.

* Header: the 7 reserved bytes and the entry padding are zero everywhere;
  the region is consumed exactly. Shared ids are word indexes from
  `xattr_blkaddr * B`, the pool directly after the superblock.
* Long prefixes: records `u16 len, u8 base index, infix` rounded to 4 bytes
  directly after the shared pool at `xattr_prefix_start * 4` (1188 = 297 *
  4); `PLAIN_XATTR_PFX` is set by `mkfs.erofs` itself whenever there is no
  packed inode. Under fragments the table lies at offset 0 of the packed
  inode, which is LZ4-compressed, and `xattr_prefix_start` is 0.
* Name filter: `~OR(1 << (xxh32(name, 0x25BBE08F + base) & 31))` over every
  attribute, shared and inline, where `name` is the attribute name without
  its built-in prefix (infix + suffix for a long prefix) and `base` the
  built-in prefix's index. 26 of 26 inodes match in every filter image; the
  stored suffix alone fails 4 of 26 with long prefixes. The filter word is 0
  with the feature off and when all 32 bits are claimed. A wrong filter
  makes the kernel report an attribute absent; `fsck.erofs` does not notice.

### E7 Chunks

`e7-chunk4k`, `e7-chunk8k`, `e7-chunk8k-again`, `e7-chunk64k`,
`e7-chunk8k-forceidx`, hand edits `e7-edit-high-part`, `-device-id`,
`-layout-upper`, `-format-0x80`, `-format-0x40`, `-block-oor`.

* Entries: 4-byte block number, hole `0xFFFFFFFF`; 8-byte `u16 high, u16
  device, u32 block`, hole `0, 0, 0xFFFFFFFF`. Index start is the inode's
  end rounded up to the entry width. Holes are made from source sparseness
  and read as zeros; identical chunks share one block within and across
  files; a wholly sparse file is one hole chunk of its size rounded up to a
  power of two.
* `chunkbits` is per inode: after writing, `mkfs.erofs` raises it to the
  largest power of two dividing every run of contiguous chunks.
* The layout word's upper 16 bits are uninitialised stack bytes: two builds
  of one tree differ only there and in the checksum. The kernel reads the
  low 16 bits only; `0x1234` in the upper half reads fine. Format bit `0x80`
  is refused (`unsupported chunk format`); bit `0x40` without `48BIT`, a
  nonzero high part without bit `0x40`, and a nonzero device identifier
  without devices are all accepted and ignored by kernel and `fsck.erofs`.
* A chunk block past the image: the kernel returns `EIO`; `fsck.erofs
  --extract` writes zeros and exits 0.

### E8 LZ4 and index formats

`e8-lz4-compact`, `e8-lz4hc-legacy` (full index), `e8-lz4-compact-xattr`
and `-counts` (348 files at every header phase, record counts 2 to 71),
`e8-lz4-big16k`, `-big64k`, `-big1m`, `e8-lz4-ztail`, `-frag`, `-allfrag`,
`-dedupe`, `-zdir`, `-zdir-legacy`, `e8-lz4-zeros40m`, `-zeros40m-big1m`,
`e8-lz4-words6m-big1m`, hand-built `e8-lz4-block-end`, `e8-lz4-unpadded`,
`e8-lz4-distance0`, `e8-lz4-flag-cleared`, `-flag-cleared-distance0`.
Every producer fixture: `fsck.erofs` and `--extract` exit 0, the kernel
reads every file equal to the manifest; the independent dumper's extents
equal `dump.erofs -e` on every file except the big-cluster cases below.

* Compact packing: the map header ends at `mpos`; `((32 - mpos mod 32) /
  4) & 7` records in 8-byte groups, then `rounddown(rest, 16)` records in
  32-byte groups under advice `0x01`, then 8-byte groups; an odd final
  record fills a whole 8-byte group. Record count `ceil(size / L)`. Advice
  `0x01` clear is never produced (source).
* Last record: over 592 indexed inodes in `e8-lz4-compact`, `-compact-counts`,
  `-lz4hc-legacy`, `-big16k`, `-dedupe`, `-ztail`, `-frag`, `-allfrag`,
  `e2-count-zt`, `e9-zstd-l3` and `e9-mixed-lz4-lzma`, the last record takes
  three forms and no other. 395 end with a marker, a plain record with
  `clusterofs = size mod L` beginning an empty cluster at `size`. 69 end
  with the last cluster's own head or plain record, with
  `clusterofs < size mod L` and no marker: the cluster starts inside the
  last logical cluster (`e9-zstd-l3` `/text026`, size 106696, record 26
  plain at `clusterofs` 165, block 215; 36 raw tails in
  `e8-lz4-compact-counts`, such as nid 121, size 21980, record 5 plain at
  1291; 30 inline tails in `e8-lz4-ztail` and 1 in `e2-count-zt`; the
  fragment of `e8-lz4-frag` nid 72310, size 20000, record 4 head-1 at 0).
  128 have `size mod L = 0` and end with the last cluster's head, plain or
  non-head record. Source: `compress.c` `z_erofs_write_full_indexes` writes a
  record for a logical cluster only while the extent reaches its end and
  clears the pending offset when the final extent fits its cluster;
  `z_erofs_fini_full_indexes` adds the marker only for a nonzero pending
  offset. `zmap.c` ends an extent at `i_size` once the next record index is
  at or past the record count. Restricted to `e8-lz4-compact`,
  `-compact-xattr` and `-lz4hc-legacy`, which have no raw tail, the tally is
  176 marker files and 86 without.
* Inline tail: the tail's `h_idata_size` bytes start at the index end
  (`zmap.c` sets `z_fragmentoff` to `nextpackoff` of the last record, the
  end of its compact group) and must lie in one block (`invalid
  tail-packing pclustersize` otherwise). All 81 inline tails of
  `e8-lz4-ztail` and both of `e2-count-zt` do; 5 of the 81 lie in the block
  after the one holding the inode (nids 73471 at `0x23dfe0` with tail at
  `0x23e030`, 72446, 72701, 72829, 75135), where `dump.erofs -e` reports the
  tail extent at that offset (nid 73471, `/text008`: physical 2351152,
  length 2762). 76 tails have a head-1 record and 5 a plain one
  (`compress.c` inlines the raw bytes when compression gains nothing). The
  tail head's block field is the producer's reserved block, not the tail
  (nid 73471 record 7 block 61; two single-cluster files both name block
  703); the fragment head's block field is likewise no address
  (`compress.c` stores `fragmentoff >> 32` there on the full index).
* Head block numbers: anchor + 1 + preceding heads in the group without big
  clusters (0 mismatches). With big clusters the kernel's backward walk
  (source `zmap.c`) adds each count marker met, including the marker of a
  cluster whose head is in the previous group: `e8-lz4-big16k` `/mixed`
  record 44 is block 25 = anchor 21 + 4, which the dumper computes as 21;
  40 further files in `e9-mixed-lz4-lzma`. A compressed cluster that is
  not a partial reference, whose extent is shorter than its stored blocks
  and does not end the file, is refused by the kernel (source, `zmap.c`,
  "too much compressed data").
* Big clusters: compact advice `0x07`, full-index advice `0x02`; the kernel
  requires compact `0x02` and `0x04` equal (source). Every cluster that
  spans a non-head record has the marker, count 1 included. Observed
  physical clusters: 4, 16 and 256 blocks for `-C16384`, `-C65536`,
  `-C1048576`; the LZ4 record is `0e00 ffff NNNN` + 10 zero bytes.
* Partial references: bit 15 of a full record only, under `-E dedupe`,
  which forces the full index (`/dup/head5` record 3 and `/zeros1m` record
  254 share clusters with other extents); no compact form exists.
* Interlacing: `-E fragments` sets advice `0x10`; every plain cluster is
  rotated, extent byte `j` at block byte `(clusterofs + j) mod B`. Without
  it plain clusters are shifted.
* Limits: `L = B` always; the kernel caps a stored cluster at 1 MiB and a
  decoded extent at 12 MiB (source). The producer's maxima: 1 MiB stored,
  8,192,000 bytes decoded, 1,041,954 bytes through one block (zeros).
* Stream termination: of ten hand-built blocks the kernel and `fsck.erofs`
  read c0 (five final literals, last match 9005 bytes before the end) and c8
  (five final literals, last match exactly 12 before the end) and refuse c1
  to c7 and c9 (one to four final literals, or a last match 11 or fewer
  bytes before the end). 1314 producer clusters all meet both with a
  minimum of five final literals and a last-match distance of 12; the
  largest offset is 65528.
* `LZ4_0PADDING`: set on every fresh build, `-E legacy-compress` included.
  With the bit cleared and streams still padded the kernel reads everything
  (it strips leading zeros regardless); with the bit cleared and `0x54` = 0
  LZ4 counts as unavailable. Left-aligned streams shorter than their cluster
  are refused by both oracles.
* Distance word `0x54`: `mkfs.erofs` writes 65535; the kernel and
  `fsck.erofs` read a volume with 0.
* Other forms: `-E ztailpacking` advice `0x09`, tail size in the header's
  high half; `-E fragments` packed NID with an LZ4-compressed packed inode;
  `-E all-fragments` whole-file fragments; `--zD` compressed directories
  as full-index whole-file fragments with bit 63 set in the header.

### E9 Other codecs

`e9-lzma-*` (levels 0, 6, 9, 109; dictionaries 64 KiB and 8 MiB; `-C65536`),
`e9-deflate-*` (levels 1 and 9; dictionaries 4096, 256 and 128;
`-C65536`), `e9-zstd-*` (levels 1, 3, 19, 22; dictionaries 4096 and 1 MiB;
`-C65536`), `e9-*-words6m-big1m`, `e9-*-zeros40m-big1m`, `e9-config-four`,
`e9-mixed-lz4-lzma`, `e9-mixed-*-meta`, hand-built `e9-head2-lzma`,
`-lz4-ctrl`, `-wrongalg`, `-head1-wrong`. Every cluster of every producer
fixture decodes with an independent library (liblzma raw LZMA1, zlib raw
inflate, libzstd) to exactly its extent: 354 LZMA, 410 DEFLATE and 467
Zstandard clusters at the defaults.

* Framing: all three right-aligned after leading zeros. MicroLZMA is the
  inverted properties byte (`0xA2` for lc 3, lp 0, pb 2) then the LZMA1
  stream without its leading zero byte and without an end mark, decoded to
  the extent length. DEFLATE is one raw final block (first byte `0x35`).
  Zstandard is one complete frame with content size, no checksum and no
  dictionary id.
* Records: one per set mask bit in increasing order, 14 bytes for LZ4 and
  MicroLZMA, 6 for DEFLATE and Zstandard, 4-byte aligned after the slots.
  MicroLZMA `dict_size` 32768, 262144, 4 MiB by cluster size, `format` 0;
  DEFLATE window bits 15, 12, 8 and 7 for dictionaries 32768, 4096, 256 and
  128; Zstandard `format` 0 and window log less 10 from 2 to 10. The mask
  lists configured algorithms only; the four records of `e9-config-four`,
  on a superblock without slots, end at byte 1200. Every non-LZ4 compact
  inode carries advice `0x07`, at one block too. Kernel bounds (source):
  MicroLZMA
  `format` 0 and dictionary 4096 to 8 MiB, DEFLATE window bits at most 15,
  Zstandard `format` 0 and window log at most 10 (2^20), each reported as
  invalid. `mkfs.erofs` refuses LZMA above 8 MiB or below 4096, DEFLATE
  above 32768 and Zstandard above 1 MiB.
* Head-2: no producer writes a type-3 record (1576 compressed inodes, all
  head-2 nibble 0). Hand-built: a type-3 record selects the algorithm in
  the high nibble of header byte 6 and the kernel and `fsck.erofs` read it
  without `COMPR_HEAD2` set; a mismatched algorithm or stream fails in both.

### E14 External devices

`e14-blobdev-b4096` (`--blobdev`), `e14-tar-index-b4096` (`--tar=i`),
`e14-tar-full-b4096` (control).

* Both device volumes set extra device count 1 and device table slot 9 (byte
  1152), incompatible `DEVICE_TABLE`; the blobdev volume also sets
  `CHUNKED_FILE`, the tar-index volume does not, although its regular files
  are chunk-based, and it uses 512-byte blocks whatever `-b` says. The
  128-byte device slot has an empty tag, the device's block count and a
  mapped block address equal to the primary block count. Every chunk entry
  carries device identifier 1.
* Mounting either image alone lists the tree and fails every read; the
  image concatenated with its blob mounts as one device and reads correctly.

## Evidence answers

| Question | Answer | Fixtures |
| --- | --- | --- |
| Superblock checksum range, seed and final inversion at each block size | `[1024, B)` for `B > 1024`, `[1024, 1024 + B)` otherwise; seed all ones, no final inversion; kernel and `fsck.erofs` agree | `e1-chk-b512` to `-b4096` |
| Largest block size any producer emits | 4096; `mkfs.erofs` refuses 8192, the kernel refuses `blkszbits` 13, 14 and 16 | `e1-geom-b*` |
| The superblock region's end | 4096 for extension slots at `B` 512, 1024 and 4096; the kernel reads configuration records beyond it, which fsimg refuses by policy | `e1-slot-base-*` |
| Block count versus image length | Equal in every producer image; the kernel ignores the count, which fsimg enforces by policy | every family |
| The build-time word | Zero except under `-E 48bit`, where it is the absolute build time | `e1-bt-*`, `e2-v1*` |
| Compact inode reserved word | `0x0C` is the time in seconds after the epoch on every volume, as the kernel reads it; `0x1C` ignored by the kernel, zero in every producer | `e1-bt-48bit`, `e2-*` |
| The time's meaning without `MTIME` | v1.2: extended inodes hold the source change time, compact inodes the build time; the kernel ignores the bit | `e2-v12-*` |
| What the inode count counts | Distinct inodes, a hard link once, plus the packed inode; v1.2 writes 0 | `e2-count-*`, `e2-v12-*` |
| Flat-inline without a tail | Never produced; a nonzero multiple-of-`B` size with layout 2 is refused by kernel and `fsck.erofs` | `e3-*` |
| An empty file's start block | 0, and the kernel does not read it | `e3-*` |
| The null flat address | `0xFFFFFFFF` is the unused start word of every flat-inline inode with no full block, in every producer; a flat inode's word is read whenever its size is nonzero, a flat-inline inode's only when it has a full block | `e3-b4096-inline`, `e3-b4096-noinline`, `e2-v1*-default` |
| Directory block padding; dot-entry placement; directory link counts | Unused bytes zero; dot entries in bytewise position; 2 + subdirectories, except link count 1 on a compact directory above 65535 | `e4-*` |
| Device-number encoding; sizes and layout words of FIFOs, sockets and devices | Linux 32-bit encoding; size 0, layout 0, FIFO and socket word 0 | `e5-kinds-*` |
| Attribute header reserved bytes; prefix table placement and units; the filter hash | Zero; plain table at `start * 4` after the shared pool, packed table at `start * 4` of the packed inode's decoded content; XXH32 over the name without its built-in prefix, seed `0x25BBE08F` + base index, every attribute counted | `e6-xattr-*` |
| Chunk entry forms, holes, shared chunks | As designed; the layout word's upper half is uninitialised and ignored; per-inode `chunkbits` | `e7-chunk*`, `e7-edit-*` |
| Compact group packing; the last record and the end marker; inline tail placement; partial references; interlacing; cluster and index limits; LZ4 stream termination; the LZ4 distance field | Packing confirmed, the big-cluster head block following the kernel's walk from the group's anchor; the last record is the marker, the last cluster's own head or plain record inside the last logical cluster, or, at `size mod L = 0`, the last cluster's final record, 592 inodes; the inline tail starts at the index end and lies in one block, not necessarily the inode's; partial references in full indexes only; rotation confirmed; 1 MiB stored and 12 MiB decoded are the kernel's limits, `L = B` always; blocks need five final literals and a last match 12 bytes before the end; distance 65535 written, 0 accepted. Open: `L > B` and compact advice `0x01` clear, unobservable with the pinned release and refused until E15 observes them | `e8-*` |
| MicroLZMA, DEFLATE and Zstandard framing, configuration records and limits; mixed head-1 and head-2 | Framings confirmed; DEFLATE window bits at most 15, MicroLZMA dictionary at least 4096 and format 0, Zstandard format 0; head-2 by hand-built images, accepted by the kernel without `COMPR_HEAD2` | `e9-*` |
| Every 48-bit field; encoded extents; writer's 32-byte records | Open; blocks stages 12, 14 and 15 | E10 |
| Kernel acceptance of writer output at 512 to 2048-byte blocks and of the writer's field choices | Open; blocks stages 10 and 11 | E11 |
| Metadata-box layout, root placement, box compression, shared attributes in the box | Open; blocks stages 13 and 15 | E12 |
| Scale | Open; blocks stage 8 | E13 |
| The fields a volume with external devices sets | Extra device count nonzero in both producer forms, `DEVICE_TABLE`, device identifier 1 in every chunk; `CHUNKED_FILE` only under `--blobdev` | `e14-*` |
