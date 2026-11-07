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
  Source (`inode.c`): the writer's start-block high word and implicit
  link count apply only to an inode with a block number above 32 bits; a
  device, FIFO or socket has none, so its extended word `0x06` is zero and
  its compact link count is explicit, format bit 4 clear, under `48BIT`
  too ([E10](#e10-48-bit-layout) observes it, `e10-48-kinds`).

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
* Source (`blobchunk.c`): `erofs_inode_fixup_chunkformat` sets bit `0x40`
  when any chunk's block number exceeds 32 bits, whatever the entry width;
  `erofs_write_chunk_indexes` then writes a 4-byte entry as the low 32 bits
  only, so the bit with 4-byte entries means lost address bits. The entry
  width is chosen first: 8 bytes with extra devices or
  `--force-chunk-format=chunk-indexes`, else 4.

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
  (`compress.c` stores `fragmentoff >> 32` there on the full index and
  switches a file to the full index when its `fragmentoff` exceeds 32 bits;
  `zmap.c` ors that field, shifted by 32, into the header's offset on a
  full index only). The end marker's block field is 0 on a full index
  (`compress.c` `z_erofs_fini_full_indexes`); a compact record stores none.
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
* Empty mask (source): `decompress.c` `z_erofs_parse_cfgs` loads one record
  per set mask bit and accepts a mask of zero; `zmap.c` then refuses any
  inode whose algorithm the mask lacks. No producer writes an empty mask.
* MicroLZMA return codes, liblzma 5.8.4, `e9-lzma-l6` `/mixed` cluster 0
  (4095 stream bytes after one leading zero, extent 12226, dictionary 8
  MiB): `lzma_microlzma_decoder` then one `lzma_code(LZMA_FINISH)` returns
  `LZMA_STREAM_END` with the output full for the exact size and for an
  inexact size equal to the output (12226, 6113 and 1 bytes, the input
  partly consumed), `LZMA_DATA_ERROR` for an exact size of 6113,
  `LZMA_OK` with the output short for a size of 12227 under either flag
  and for the input cut one byte short. The reference decoder
  (`decompress.c`) accepts only `LZMA_STREAM_END`, partial or not.

### E10 48-bit layout

Producer images under `-E48bit` (`--mkfs-time`, epoch 1700000000, fixed
UUID): `e10-48-small` (`-b4096`: empty, 1-byte, 600-byte and 20000-byte
files, hard links, symbolic links, a FIFO, a directory of 120 entries, times
in 1950 and 2110 and with nanoseconds), `e10-48-dotomit` (the same tree,
`-E48bit,dot-omitted`), `e10-48-large` (`-b512 --dsunit=2^30`, start blocks
2^30 to 5 * 2^30 and 2^32, 5368709123 blocks, sparse), `e10-48-kinds`
(every kind, compact at the root and extended under `ext/` with owner
70000, built in the VM), `e10-48-lz4` and `-lz4-b512` (`-zlz4
-E48bit,dedupe`, 17 compressed files), `e10-48-deflate`, `e10-48-frag`
(`-E48bit,fragments`), `e10-48-lz4-big` (one 5 GiB + 3072-byte file, sparse)
and `e10-48-chunk` (`-E48bit,force-chunk-indexes --chunksize=1024 -b512`,
its data moved by hand to block 2^32 + 5). Hand-built images through the
independent LZ4 encoder (`test-codec --encode-lz4`, legacy frame stripped):
`e10-hand-lz4-padded`, `-exact` and `-bytes` (one 49821-byte file in four
extents, LZ4 and plain, appended to a producer base with a new extended
inode of 32-byte records) and `e10-hand-box32` (a producer's LZ4 box
re-encoded in three extents behind an extended box inode of 32-byte
records). Byte edits of producer images: `e10-edit-small-wide` (wide
superblock form on a small volume), `-small-nlink1` (bit 4 on every compact
file and symbolic link with zero high bits), `-kinds-extbit4` (bit 4 on
every extended regular file), `-lz4-no48` (`48BIT` cleared on an
encoded-extent volume) and `-lz4-counthi` (extent count high word 1 on a
16-byte-record inode). Oracles: kernel mount with SHA-256 of every file
against the manifest, `ls -aU`, `stat`; `fsck.erofs` with `--extract`;
`dump.erofs -e`; an independent walker over the raw fields. Every producer
fixture mounts, matches its manifest and passes `fsck.erofs`. One kernel
was tried; the minimum kernel for encoded extents, 6.15 by `dump.erofs`'s
banner, was not.

* **Superblock forms.** The producer writes the narrow form under
  `-E48bit`: `0x0E` is the root NID, `0x70` is 0 and the block count is
  32-bit (`e10-48-small`, `e10-48-lz4-big`, `e2-vm-48bit`). The wide form,
  `0x70` the root NID and `0x0E` the block count's bits 32 to 47, appears
  only when the count passes 32 bits or the root NID passes 16
  (`e10-48-large`: `0x24` = `0x40000003`, `0x0E` = 1, `0x70` = 36; source
  `super.c`). The kernel mounts both, the wide form on a small volume
  (`e10-edit-small-wide`, same tree hash) and a wide block count above the
  image's size (no size check at mount). The build-time word is the
  absolute build time, as E1.
* **Start-block high word.** `e10-48-large`: a compact file at 2^30 and
  3 * 2^30 keeps bit 4 clear with `0x06` = link count 1; the file at 2^32 is
  compact with bit 4 and `0x06` = 1 as start bits 32 to 47; the hard-linked
  pair at 5 * 2^30 is extended with `0x06` = 1 and the link count at `0x2C`.
  The kernel reports link counts 1 and 2 and inode numbers equal to the
  NIDs (85899345956). Source `inode.c`: bit 4 is set only on a compact
  inode whose start exceeds 32 bits; a compact inode with another link
  count and a wide start is rewritten extended. An extended flat-inline
  inode with no full block carries `0x06` = `0xFFFF` and `0x10` =
  `0xFFFFFFFF`, the 48-bit null start (`e10-48-kinds`, `ext/one`, `ext/hl`,
  `ext/slink`, `ext/sub`, `/ext`); extended inodes with data blocks, empty
  files and special files carry 0, and a compact inode keeps its explicit
  link count beside `0xFFFFFFFF`. Special files, compact and extended:
  `0x06` = 1 or 0, bit 4 clear, the device number in `0x10`
  (`0xFFFFFFFF` for 4095:1048575). No producer run placed a directory
  beyond block 2^32: `mkfs.erofs` fails with "Input/output error" when a
  multi-block directory lies past 2^32 under `--dsunit`, cause not found,
  and `--dsunit=4294967296` is silently ignored.
* **Bit 4 by edit.** Bit 4 with zero high bits on every compact file and
  symbolic link mounts with link count 1 and the same tree hash
  (`e10-edit-small-nlink1`). Bit 4 on every extended regular file mounts
  with link counts unchanged (`e10-edit-kinds-extbit4`): the kernel ignores
  the bit on an extended inode, as `erofs-utils` does by source; no
  producer sets it.
* **Compact times.** With `--mkfs-time` the compact time word is the file
  time modulo 2^32: 2110-01-01 is stored as 123039111 and 1950 as
  3663844096, nanoseconds dropped; the kernel reports those words as
  seconds after the epoch (`e10-48-small`). Source: the compact-form choice
  compares only the time without the 48-bit layout. The design's rule, a
  whole second within 2^32 seconds of the epoch, is stricter and right.
* **Omitted dots.** `-E dot-omitted` implies `48BIT`. A directory carries
  bit 4 compact with its link count explicit at `0x06` (`/dir`, format
  `0x0014`, link count 3), its data has `..` first and no `.` (`/dir/sub`:
  30 bytes against 43 with the dot). `ls -aU` prints `.. leaf .`: the
  kernel appends a synthetic `.` after the stored entries, and with bit 4
  set on a directory that stores `.` it lists `.` twice (`e10-48-dotomit`,
  a hand-edited `e10-48-small`).
* **Chunks.** The producer writes chunk format `0x0021` under `-E48bit`
  and reaches no block above 2^32 in a chunk file (`--dsunit` aligns only
  the first blob and 4-byte arrays cannot carry it). `e10-48-chunk`
  hand-moves the data to block 2^32 + 5 as source `blobchunk.c` writes it:
  entries `01 00 00 00 05 00 00 00` (high word at 0, device at 2, low word
  at 4), chunk format `0x0061`, wide superblock. The kernel reads both
  files to the manifest; `fsck.erofs` is clean and `--extract` byte-equal.
  The control with bit 6 clear was not run. The layout word's upper half
  is uninitialised, as E7.
* **Index form per file.** Under `-E48bit` the producer still writes
  legacy compact indexes (layout 3, advice `0x1` as the 2-byte-group flag)
  and full indexes when the encoded map would be larger (`e10-48-lz4`:
  `half-rand-1m`, `rand-zero-mix` compact; `half-rand-40k` encoded, width
  4); a whole-file fragment keeps the legacy map header.
* **Encoded-extent widths.** All four occur: 4 when every extent has the
  same logical length and the physical run is contiguous (`half-rand-40k`,
  `small-mix`, `tutu`); 8 when uniform but not contiguous (`xyxy`, `xyyx`
  under `dedupe`); 16 otherwise up to 4 GiB (`text200k`, `mixed`,
  `half-rand-1m` at `-C65536`); 32 beyond 4 GiB (`e10-48-lz4-big`, 961
  records, extended inode). The 8-byte header sits at the first 8-aligned
  offset after the inode and its attributes. For 16 and 32 bytes: count low
  word at 0, advice at 4 (`0x0005` and `0x0007`), count high half at 6. For
  4 and 8 bytes: bytes 0 to 3 zero, advice `0x0001` or `0x0003`, byte 6
  zero, byte 7 the logical cluster shift above the block shift (`-C65536`:
  `00 00 00 00 01 00 00 04`), no count: `ceil(size / (B << byte 7))`
  extents (source `z_erofs_write_mapheader`). Records start at the header's
  end rounded up to the record size: inode + 40, 40, 48 and 96. Record
  fields: 32 bytes, length word, physical low and high, logical low and
  high, 12 zero bytes (record 1 of the 5 GiB file: 32768 bytes LZ4 at
  40960, logical 8192000); 16 bytes, length word, physical low and high,
  logical low only; 8 bytes, length word and a 32-bit physical offset,
  logical offset the index times the logical cluster; 4 bytes, length word
  only, preceded by a 64-bit physical start at the records' offset
  (`xyxy` at `-C65536` with `--dsunit`: 4294967296), each extent starting
  where the last ended. Length word: low 21 bits the stored length, bits 28
  to 31 the format (0 plain, 1 LZ4, 3 DEFLATE in `e10-48-deflate`), bit 27
  the partial reference, set by `-E dedupe` on LZ4 extents that decode a
  prefix of a shared stream (`e10-48-lz4-b512`, `/t10-d/tutu`: 16-byte
  records 1 and 3 both name the 4096-byte LZ4 stream at byte 6144 and
  decode 16369 bytes of it, records 0 and 2 the stream at 2048). Every produced offset and length is a block multiple;
  byte-granular values never appeared with any codec or `--max-extent-bytes`.
  The layout word of an encoded inode is the compressed block count
  (`0x1543` for the 5 GiB file), `0x06` its high half by source, 0 here.
* **Fragments.** `e10-48-frag`: advice `0x35`; the last 16-byte record is
  the fragment, its length word the packed-inode offset's low 32 bits with
  no format bits (`/mixed`: 12751), its physical low word the high 32 bits,
  and it runs to the size. Whole-file fragments (`text1k`, `text5k`) use the
  legacy header; `rand100k` is compact with advice `0x37`. The kernel and
  `fsck.erofs` read all nine files.
* **Feature pairing and count by edit.** Clearing `48BIT` on `e10-48-lz4`
  (`e10-edit-lz4-no48`) mounts with the same tree hash and passes
  `fsck.erofs`: the kernel reads encoded extents, 4- and 8-byte widths
  included, and omitted dots without the bit. A count high word of 1 on
  `half-rand-1m` (`e10-edit-lz4-counthi`) mounts and reads the file at its
  full size with wrong content from byte 1 (522184 bytes differ);
  `fsck.erofs` exits 1. The kernel uses the high half and never checks the
  count against the body.
* **Hand-built 32-byte records.** The four-extent file (LZ4, plain, LZ4,
  LZ4 over 49821 bytes) in three placements, block-aligned and zero-padded
  as `lz4_0padding` lays it out (40960/8192, 49152/12000, 61440/8192,
  69632/8192), block-aligned with exact stored lengths (5219, 12000, 7151,
  7279) and contiguous from an unaligned start (40997/5219, 46216/12000,
  58216/7151, 65367/7279): the kernel reads the file to the expected
  SHA-256, `fsck.erofs` exits 0 and `--extract` is byte-equal, `dump.erofs
  -e` lists the four extents through the new NID. The box re-mapped by
  three 32-byte records (`e10-hand-box32`, box inode NID 1408) mounts with
  the original's 130 files and tree hash and extracts equal under
  `fsck.erofs`; setting its first record's format to plain makes both
  refuse the volume (`bogus i_mode (0)` at the box root), so the box was
  read through the new records. A flipped LZ4 data byte inside a block
  changes content undetected by either tool.
* **Open.** A length word of zero; a directory with a start block above 2^32;
  the bit-6 control on `e10-48-chunk`; a 16-byte record with a nonzero
  physical high half; a produced count above 2^32; kernels other than
  7.0.0-38.

### E11 Writer probes

The question was whether the kernel accepts the writer's output at 512,
1024 and 2048-byte blocks and the writer's own field choices: an empty
inode's start word of 0, the unused start word 0 of a flat-inline inode
under one block (producers write `0xFFFFFFFF`), inodes that end exactly at
a block's end or move to the next block, directories spanning blocks, and,
at `B` 512, a superblock checksum range `[1024, 1536)` that holds the first
inodes. The plan's hand-built variants were not needed: the writer's
layout is already parameterized by `B`, so its refusal of the smaller sizes
was lifted and its own output qualified at each size.

On 2026-11-07, `fsimg` built from branch `erofs` at 340c74ea ("Register
and qualify the EROFS writer") with the block-size refusal removed from
`validate_options()` and the descriptor's write mask widened to the four
sizes, `fsck.erofs` 1.9.4-gf36cadb5 and rust-fs-erofs 0.5.1
on macOS, and the colima VM's kernel `7.0.0-38-generic` ran stage 10's
qualification once per block size:

```sh
for b in 512 1024 2048 4096; do
    sh tools/erofs/qualify.sh -b $b build/fsimg \
        ~/.cache/fsimg-erofs/vm-e11/run2/b$b /Volumes/erofs-e11/work/scratch-$b
done
```

`-b` passes `-o cluster=B` to every output. Each size ran the outputs and
oracles of [fsimg writer output](#fsimg-writer-output), with two changes
to the trees: `small` adds files of `B - 32`, `B - 1`, `B` and `B + 1`
bytes, and a new `tails` tree holds files of `B + 32i` bytes for `i` from 1
to `B / 32 - 2`, so the extended inodes carrying their tails grow to fill
a whole block, some ending exactly at a block's end and some moving to
the next. A structural walk of every
image but `large` (a scratch script outside the repository that follows
the root's directory tree with its own decoder) counted the field choices
the kernel then read, summed over the nine images, the copied-out `gpt` and
`mbr` partitions included; no flat-inline inode used `0xFFFFFFFF`:

| `B` | Inodes | Inodes ending at a block's end | Multi-block directories | Empty inodes, start word 0 | Flat-inline inodes under one block, start word 0 |
| --- | --- | --- | --- | --- | --- |
| 512 | 1666 | 191 | 2 | 1524 | 96 |
| 1024 | 1682 | 97 | 2 | 1524 | 96 |
| 2048 | 1714 | 48 | 2 | 1524 | 97 |
| 4096 | 1766 | 25 | 2 | 1524 | 97 |

Inodes also moved past block ends: at 512 bytes, `deep`'s 42 inodes end at
no block's end, yet ten of them start a block.

| `B` | fsck | Kernel | Rust | fsimg | Outputs |
| --- | --- | --- | --- | --- | --- |
| 512 | Pass | Pass | Pass | Pass | All ten |
| 1024 | Pass | Pass | Pass | Pass | All ten |
| 2048 | Pass | Pass | Pass | Pass | All ten |
| 4096 | Pass | Pass | Pass | Pass | All ten |

The ten outputs are `empty`, `small`, `deep`, `wide`, `large`, `kinds`,
`tails`, `names`, `erofs/gpt` and `erofs/mbr`; each size also refused
`cluster=8192` as outside the writer profile. Every output at every size
passed every oracle, 160 checks with none failed, and the kernel logged
nothing for these mounts but the root NID. The answer: the kernel,
`fsck.erofs` and rust-fs-erofs accept the writer's output at 512, 1024,
2048 and 4096-byte blocks, with its empty-file and flat-inline start words
of 0, inodes ending at and moving past block ends, and multi-block
directories. The writer accepts all four sizes.

### E12 Metadata box

Producer images from `mkfs.erofs` 1.9.4 built in the VM (the source trees
carry root-owned attributes and an ACL; `-T0`, fixed UUID), each from a
135-entry tree with two directory levels, a 120-entry directory over one
block, `user.common=samevalue` shared by `c1` to `c6` and `zcommon/z1` to
`z6`, an ACL on `c1`, two `user.vendor.alpha.*` names, a compressible
20000-byte file and a 9000-byte random one: `e12-box-plain` (`-b4096
-m4096`), `e12-box-lz4` (`-zlz4 -m4096`), `e12-box-lz4-b1024`,
`e12-box-plain-b512`, `e12-box-lz4-m16k` (`-C16384 -m16384`),
`e12-box-pfx-plain`, `-pfx-lz4`, `-pfx-plain-b512` and `-pfx-lz4-plainpfx`
(`--xattr-prefix=user.vendor.alpha.`, the last with
`-Eplain-xattr-prefixes`), `e12-box-lz4-frag` and `-lz4-pfx-frag`
(`-Efragments`), `e12-box-chunk` (`--chunksize=4096`), `e12-nobox-lz4`
(control) and `e12-flat-box-plain` and `-lz4` from a flat 100-entry tree
whose root fits inline. Hand-patched from producer images, checksum
recomputed: `e12-box-plain-poolbox` and `e12-flat-box-poolbox` (one block
appended to the box holding a copy of the shared entry at in-block offset
`4 * id`, `xattr_blkaddr` pointing at it, compat `0x08` set);
`e12-flat-box32-plain` and `-lz4` (`48BIT` cleared, the root inode with its
attributes and inline tail copied from the box to primary slot 48, its `.`
and `..` rewritten, the 16-bit root field set, the wide field zeroed; the
flat tree so no child's `..` names the old root); `e12-flat-box32-poolbox`
(both patches). Oracles: kernel mount with content, `stat` and `getfattr
-d` compared per path with the source tree; `fsck.erofs` with `--extract`;
`dump.erofs --cat --nid` of the box; a walker over every inode. Every one
of the 20 fixtures mounts and passes `fsck.erofs` with no `<E>`; content and
`stat` match the source on every path.

* **Superblock and box inode.** Every `-m` image has one extension slot,
  `METABOX 0x100` with `48BIT 0x80` (incompat `0x180`, `0x181` with LZ4),
  compat `0x07`, `metabox_nid` 38 at `0x80` with bit 63 clear, the wide
  root `0x8000000000000000`, `meta_blkaddr` 0; `dump.erofs` names kernel
  6.17 as the minimum. The box inode is NID 38 rather than 37 because the
  20-byte shared entry sits at byte 1168 of the primary space before it
  (NID 37 in `e13-limits-metabox`, which has no shared entry). The plain
  box is a compact flat inode (layout 0) of 12288 bytes occupying the last
  three blocks of the image, its content zero-padded to a block multiple;
  the LZ4 box is layout 1 of 8192 bytes with one stored block, mapped by
  4-byte encoded extents with advice `0x0001` and logical cluster shift 1
  (header `00 00 00 00 01 00 00 01`), the same form as a regular LZ4 file
  in the 48-bit image, and the producer
  stores the box flat when one block would not shrink. `-m` needs a
  positive size, so the producer writes no plain box beside compressed
  files; `--MZ` is a different mechanism without a feature bit (source
  `metabox.c`).
* **What the box holds.** Every inode but the box inode and the packed
  inode (NID 40 in `e12-box-lz4-frag`): 142 in the plain image, the LZ4
  file in `e12-box-lz4`, 16 chunk inodes in `e12-box-chunk`. Every
  directory entry carries bit 63 (`c1` is `0x8000000000000008`).
  Directory blocks beyond the inline tail, file blocks and chunks are in
  the primary volume. Of the inline tails checked at 512, 1024 and 4096
  bytes none crosses a block multiple inside the content; one inode's
  header and attribute region straddle a block at 512 bytes (box NID
  `0x3f`), its tail inside the next block. `dump.erofs --cat` decodes the
  box to the same bytes, plain and compressed.
* **Long-prefix table.** Without `-Eplain-xattr-prefixes` the table is in
  the box content at `xattr_prefix_start` 0, with or without a packed
  inode (`e12-box-pfx-plain`, `-pfx-lz4`, `-lz4-pfx-frag`; compat `0x07`,
  no `PLAIN_XATTR_PFX`, which a box volume does not get by default): bytes
  `0e 00 01 "vendor.alpha."`, zeros to the end of box block 0, the root at
  NID bit 63 plus 128 at 4096 bytes and plus 16 at 512. The kernel returns
  the long-prefix names. With the option the table is plain at 297 * 4
  after the pool and the root is the box's first inode. Source
  `erofs_xattr_flush_name_prefixes` and kernel `xattr.c`: the table goes to
  the box and is read from it under `METABOX` without the plain bit, never
  from the packed inode.
* **Shared attributes.** No site in `erofs-utils` 1.9.4 sets
  `SHARED_EA_IN_METABOX`; the pool stays in the primary volume at
  `xattr_blkaddr * B + 4 * id` (id 292 at 4096 bytes, 36 at 512). Kernel
  7.0 with that layout: content, `stat` and mode right on every path, but
  `getfattr -d` wrong on exactly `c1` to `c6` in `e12-box-plain`,
  `-plain-b512`, `-lz4`, `-lz4-b1024`, `-lz4-frag`, `-box-chunk`,
  `-lz4-m16k`, `-pfx-lz4-plainpfx` and the flat images: `user.common`
  missing and garbage names (`F`, `G`, `user.`) with "Operation not
  supported", while `user.own` and the ACL show; `zcommon/z1` to `z6`,
  which reference the same shared entry, are right. `c1` to `c6` sit in box
  page 0 (offsets 288 to about 600), `z1` in page 2; where the prefix table
  occupies box block 0 and moves every inode one block later
  (`e12-box-pfx-*`), all 141 paths are right, as they are in the control
  without a box. Cause (source, a hypothesis consistent with every
  observation): the kernel's attribute code switches one cached metadata
  buffer between the box mapping, for inline entries, and the primary
  mapping, for the pool, through `erofs_init_metabuf`, which changes the
  mapping without dropping the cached page, and `erofs_bread` reuses the
  page when the index matches (v6.17 `xattr.c:331, 372`; v7.0 `xattr.c:266,
  307`; `data.c`); the pool is primary page 0, so a box inode whose
  attribute area is in box page 0 reads the wrong page. No mount failure,
  no log line. This is a kernel defect, its cause a hypothesis from source
  reading, not a reason to place the pool elsewhere; the design keeps the
  producer's placement. With the pool
  moved into the box under compat `0x08` (`e12-box-plain-poolbox`,
  `e12-flat-box-poolbox`, plain boxes) the kernel's attributes equal the
  source on all 141 and 100 paths and `fsck.erofs` is clean, so that
  placement is read as the design states; a pool in a compressed box was
  not built. Compat `0x08` on a volume without `METABOX` mounts, and every
  shared attribute read fails `Structure needs cleaning`; `fsck.erofs` does
  not notice.
* **Box without 48BIT.** `e12-flat-box32-plain`, `-lz4` and `-poolbox`
  mount with `mounted with root inode @ nid 48` and only the metadata
  compression line in the log; content and `stat` match, attributes as
  their base; the entries still carry bit 63; `fsck.erofs` exits 0 and
  `--extract` yields the 100 entries. The `-lz4` image's box is mapped by
  encoded extents without `48BIT`, which the kernel reads and the design
  refuses by its feature-pairing policy. Kernel `super.c` (v6.17 and v7.0)
  takes the root from the 16-bit field unless `48BIT` and the wide field
  is nonzero and does not require `48BIT` for `METABOX`; `mkfs.erofs`
  sets both because the root is in the box (source `inode.c`). No producer
  writes the form.
* **Refusals.** `metabox_nid` with bit 63: mount fails `Structure needs
  cleaning` and `fsck.erofs` fails to read the superblock (source
  `super.c` self-loop check). No extension slot with `METABOX`: the same.
  A directory entry with bit 63 in a volume without the box: mount
  succeeds, `ls` lists the entry, reading it fails with `-117`; `fsck.erofs`
  reports an I/O error on the inode.
* **Open.** A pool in a compressed box; a packed NID with bit 63; a box
  inode with the flat-inline, chunk-based or compact-index layout, which
  the producer never writes.

### E13 Scale

`e13-big5g`, `e13-big5g-chunk1m`, `e13-big5g-lz4` (one file of 5 GiB +
3072 bytes, holes with six tags at 0, 4 GiB - 4096, 4 GiB, 4 GiB + 8192,
5 GiB and the last KiB, and a 6-byte file; plain, `--chunksize=1048576`
and `-zlz4`, all `-b4096`), `e13-dir100k` (`/d` holding `f00000` to
`f99999`, empty, beside `sib/small.txt` and `top.txt`), and four limits
images: `e13-limits-startblk` (`-b512 --dsunit=2147483647`),
`e13-limits-nid` (`-b512 --dsunit=1073741824`), `e13-limits-48bit` (`-b512
-E48bit --dsunit=2147483648`) and `e13-limits-metabox` (`-b4096 -m4096`).
`mkfs.erofs` never writes the gap `--dsunit` opens, so the limits images
are 1.6 to 3.3 TiB apparent and a few KiB allocated; the plain 5 GiB image
was made sparse after capture (its stored zero blocks skipped, content
unchanged). The last five were captured with `capture-sparse.rb`, a copy of
`capture.rb` kept with the evidence scripts, whose `sha256.txt` holds a
sparse digest (each allocated extent's offset, length and bytes, holes zero)
and which records the attribute column of the 100000 empty files as
`unavailable`. Oracles: kernel mount with `find`, `stat` and SHA-256 against
the manifest; `fsck.erofs` and `dump.erofs`.

* Files beyond 4 GiB need no 48-bit layout. Plain: `/big5g.bin` is an extended
  flat-inline inode of size `0x140000C00`, data blocks 1 to 1310720 and a
  3072-byte inline tail at `0x140001040`; the inode follows its data (NID
  167772288). Chunked: 5121 four-byte entries with `chunkbits` 8; the chunks
  holding a tag and the 4096-byte last piece are data blocks, the rest holes
  `0xFFFFFFFF`, 777 blocks in all. LZ4: a compact index of 1310721 records
  with 4096-byte logical clusters, on-disk size 22286336. The kernel reads all
  three to the manifest's SHA-256.
* `/d` in `e13-dir100k` is a compact flat-inline directory of size
  1804418: 440 full blocks and a 2178-byte inline tail, 100002 entries with
  the dots, names in byte order, about 227 entries a block. Its children are
  compact inodes from NID 115; the inode count is 100005.
* The largest 32-bit start block is `0xFFFFFFFE` (`b.bin` in
  `e13-limits-startblk`, block count `0xFFFFFFFF` of 512 bytes);
  `0xFFFFFFFF` stays the no-block marker of an inline-only inode.
* NIDs above 2^32 occur without the 48-bit layout: the file inodes of
  `e13-limits-nid` follow their data at NIDs 51539607696 to 51539607728,
  and the kernel reads them, its inode numbers equal to the NIDs. The NID
  formula needs no 32-bit limit.
* Under `-E48bit` the start blocks reach 2^32 and `3 * 2^31`, the block
  count's high word at `0x0E`, the root NID in the wide field at `0x70`;
  `-m4096` puts the root at NID `0x8000000000000000` (bit 63, the metadata
  box). The kernel reads both, and `fsck.erofs` the 48-bit image.
* The root NID is 36, 128 under `--MZ` or a large root attribute area, or
  the bit-63 box root: `mkfs.erofs` allocates the root before other
  metadata and moves `meta_blkaddr` only when the root's offset exceeds
  `0xFFFF * 32` bytes, which no input reaches (source). A root NID above
  65535, and a nonzero `meta_blkaddr`, come only from hand-built images.

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

### E15 Large logical clusters, compact advice `0x01` clear, interlaced inline tail

Hand-built one-file LZ4 volumes: a `mkfs.erofs -T0 -zlz4 -Elegacy-compress`
skeleton whose file inode, map header and index are re-laid by the
experiment's own encoder, each segment compressed by `mkfs.erofs` on its
own, the superblock checksum recomputed. Oracles: kernel mount with
SHA-256 against the source, `fsck.erofs --extract`, `dump.erofs -e`.
Mounting never fails; the header and records are read at the first file
read.

* `L > B` (`e15-a-*`, full and compact at `L = 2B` and `4B`, `B` 2048 and
  4096): the kernel and both tools read every image whose compact groups
  are laid out for `ceil(size / B)` records, the kernel's `totalidx`
  (source `zmap.c`), while reading only `ceil(size / L)` of the records,
  the last holding the file's last byte; a record's `lcn` covers
  `[lcn * L, (lcn + 1) * L)`, a head's `clusterofs` is below `L` and
  distances count lclusters. The same file with groups laid out for the
  `L` count fails: kernel `EUCLEAN`, `fsck.erofs` "failed to full
  decompress", `dump.erofs` "failed to get file blocks range"
  (`e15-a-compact-b2048-l4096-adv1-lcount`). Compact fields are
  `max(lclusterbits, 12)` bits wide, 13 and 14 observed; with advice `0x01`
  set and `L > 4096` a record in a 32-byte group is `EOPNOTSUPP`
  (`e15-a-compact-b4096-l8192-adv1flip`), so a compact inode with
  `L > 4096` needs `0x01` clear. `mkfs.erofs` sets `0x01` only for
  `lclusterbits <= 12` and uses the full index above 14 (source).
* Advice `0x01` clear (`e15-b-compact-*`, `L = B`, `B` 2048 and 4096,
  non-head records in every slot): every record sits in an 8-byte group
  from the header's end, no initial run, no 32-byte group; the kernel and
  both tools read them and `dump.erofs -e` lists the extents as built. The
  index is up to twice as long as under `0x01` set, so it cannot be
  re-laid in place.
* Inline tail position at `L = 2B` (`e15-c-l2b-tail-at-L`, `-at-B`,
  `L = 8192`, a 9000-byte segment and a 1500-byte plain tail): the tail
  starts after the record holding `lcn = (size - 1) >> lclusterbits`,
  `ceil(size / L)` records on the full index; placed after
  `ceil(size / B)` records instead, the kernel and `fsck.erofs` both read
  garbage. Source (`z_erofs_map_blocks_fo`, `nextpackoff` of the last
  byte's lcluster) says the same for the compact index, not observed.
* Plain inline tail under interlacing (`e15-c-il-*`, `B = L = 4096`, a
  tail of `E` bytes at `clusterofs` `C`, advice `0x18`; `e15-c-shift-*`,
  advice `0x08`, as controls): `mkfs.erofs` 1.9.4 cannot produce the
  combination, since `may_inline` requires `!may_packing` and
  `may_packing` holds for the final extent of every file under
  `fragments`, which also sets `0x10` on every file (source `compress.c`);
  51 files under `-E ztailpacking,fragments` gave only whole-file
  fragments and compact inodes with advice `0x31`. The three shifted
  controls read correctly. Interlaced, the kernel applies the interlaced
  decoder's rotation to the `E` stored bytes: output `[0, B - C)` comes
  from input offset `E - (B - C)`, the rest from the input's start. With
  `E > B - C` it therefore reads the extent stored rotated left by `B - C`
  correctly (`e15-c-il-rot-wrap`) and the extent stored as is rotated
  (`e15-c-il-plain-wrap`); with `E <= B - C` the input offset precedes the
  tail, so the file receives the metadata bytes before the tail in the
  inode's block, differing between runs (`e15-c-il-nowrap`,
  `e15-c-il-short`), and no stored form reads correctly. `fsck.erofs`
  1.9.4 reads bytes from after the inline data, in the following block,
  in every interlaced case and never agrees with the kernel.

## fsimg writer output

`tools/erofs/qualify.sh` ran on 2026-11-07 against `fsimg` built from
branch `erofs-s10` at "Refuse EROFS volumes below one block and
free-space requests", with
`fsck.erofs` 1.9.4-gf36cadb5 and rust-fs-erofs 0.5.1 on macOS and the
colima VM's kernel `7.0.0-38-generic`:

```sh
sh tools/erofs/qualify.sh build/fsimg ~/.cache/fsimg-erofs/vm-q/run \
    /Volumes/erofs-s10/scratch
```

Each image was created with `fsimg c -p -o uuid=... -F erofs`, so it holds
the source's owner, permission bits and modification time. Each check
compares a manifest of the image as the oracle reads it with one of the
source tree: per path the kind, permission bits, owner, group, size of
files and links, modification time in nanoseconds, SHA-256 of files and
link target.

| Oracle | Verdict rule | Fields compared | Cannot check |
| --- | --- | --- | --- |
| `fsck.erofs --extract --preserve` | Exit 0 and no `<E>` line | All but a link's mode | A link's mode, since macOS gives an extracted link its own; the root, which is the extraction directory |
| Kernel loop mount (`ro,nodev,nosuid,noexec`), `find -printf` and `sha256sum` as root | Mount succeeds, the listing matches | All | Inode numbers, link counts and directory sizes, which no source fixes |
| rust-fs-erofs `fs IMAGE stat` per path, `export --recursive --preserve=links` | Every export completes, the listing matches | All | As the kernel |
| fsimg `W`, `t --json` | `W` exits 0, the listing matches | All but contents | Contents, which `W` checks against the image's own structure only |

No oracle compares the root's metadata: the root has no catalog entry and
the writer gives it owner 0, mode 0755 and the format time. Extended
attributes, device nodes, sockets and hard links are not checked because
the writer stores none: a host source refuses FIFOs, devices and sockets,
and hard links become independent files, which every oracle reads back
with the source's content. The FIFO kind, which a library catalog can
supply, is covered by the driver contracts, not here.

| Output | Source | fsck | Kernel | Rust | fsimg |
| --- | --- | --- | --- | --- | --- |
| empty | The root alone | Pass | Pass | Pass | Pass |
| small | Files of 0, 1, 31, 32, 4064, 4095, 4096, 4097 and 9000 bytes, a 2000 time, modes 0600 and 0750, a link | Pass | Pass | Pass | Pass |
| deep | 40 nested directories and a file | Pass | Pass | Pass | Pass |
| wide | 1500 entries in one directory, 20 names of 255 bytes in another: multi-block directories | Pass | Pass | Pass | Pass |
| large | A sparse file of 2^32 + 5000 bytes (an extended inode, flat-inline) and a 1 MiB + 1 file | Pass | Pass | Pass | Pass |
| every kind | Empty file and directory, regular file and its hard link, short, absolute, dangling and 1000-byte links | Pass | Pass | Pass | Pass |
| names before `.` | Names starting with space, `!`, `#`, `$`, `%`, `&`, `'`, `(`, `)`, `*`, `+`, `,`, `-`, `.hidden`, `..dots`, UTF-8 | Pass | Pass | Pass | Pass |
| `erofs/gpt`, `gpt:disk=8M` | small; the 7323136-byte partition ends in a partial block, copied out for the oracles | Pass | Pass | Pass | Pass |
| `erofs/mbr`, `mbr:disk=8M` | small, copied out likewise | Pass | Pass | Pass | Pass |

No oracle rejected an output. This run covered 4096-byte blocks only;
[E11](#e11-writer-probes) repeats the qualification at every block size,
with the `tails` tree added.

### LZ4 output

`tools/erofs/qualify.sh -c lz4` ran on 2026-11-07 at each block size
against `fsimg` built from branch `erofs-s11b` at "Write LZ4-compressed
EROFS files under compression=lz4", with `fsck.erofs` 1.9.4-gf36cadb5 and
rust-fs-erofs 0.5.1 on macOS and the colima VM's kernel
`7.0.0-38-generic`:

```sh
for b in 512 1024 2048 4096; do
	sh tools/erofs/qualify.sh -b $b -c lz4 build/fsimg \
	    ~/.cache/fsimg-erofs/vm-11b/b$b
done
```

Every output of the uncompressed run was created again with
`-o compression=lz4`, under the same oracles and comparisons, and a
`mixed` tree was added: text files of 1, `B - 1`, `B`, `B + 1`, `2B`,
`3B + 5`, 65535, 65536, 65537, 300000 and 1000003 bytes, noise files of
`B + 1`, `2B` and 100000 bytes, 100000 bytes of text followed by 100000 of
noise, 5000 bytes of noise followed by 200000 of text, a 70000-byte
period-251 pattern and a sparse file of 3000000 zero bytes. So the images
hold LZ4 clusters, plain clusters inside compressed files, files that stay
flat, end markers, last clusters whose head lies in the last logical
cluster and, at 512 bytes, full indexes that cross blocks. All 45 checks
passed at each of the four block sizes; no oracle rejected an output.

Image sizes in bytes against the same tree without compression (the
`fsimg c -n` plan):

| Output | `B` | Uncompressed | LZ4 |
| --- | --- | --- | --- |
| mixed | 512 | 5081600 | 336384 |
| mixed | 1024 | 5087232 | 335872 |
| mixed | 2048 | 5099520 | 405504 |
| mixed | 4096 | 5124096 | 573440 |
| small | 512 | 31232 | 8704 |
| small | 4096 | 28672 | 24576 |
| large | 512 | 4296022528 | 101666816 |
| large | 4096 | 4296024064 | 276901888 |

The large tree's sparse file of 2^32 + 5000 bytes stores one block per
64 KiB window, the writer's cap on a cluster's input, so its compressed
size grows with `B`; at 512 bytes half of it is the full index, one 8-byte
record per logical cluster. Trees with no file of two or more blocks, such
as deep, wide and names, are the uncompressed image byte for byte.

### 48-bit output

`tools/erofs/qualify.sh -a 48` ran on 2026-11-07 at each block size,
uncompressed and under `-c lz4`, against `fsimg` built from branch
`erofs-s14` rebased on the 48-bit reader ("read the EROFS 48-bit
layout"), with `fsck.erofs` 1.9.4-gf36cadb5 on macOS and the colima VM's
kernel `7.0.0-38-generic`:

```sh
for c in none lz4; do
	for b in 512 1024 2048 4096; do
		sh tools/erofs/qualify.sh -a 48 -b $b -c $c -x rust \
		    build/fsimg ~/.cache/fsimg-erofs/vm-14b/a48-$b-$c
	done
done
```

Every output was created with `-o addressing=48`: the superblock's wide
form, every directory extended, the implicit link count on every compact
file and link, times as offsets from the smallest modification second
and, under `lz4`, compressed files mapped by 32-byte encoded extents with
LZ4 and plain extents. Under `-a 48` the small tree also holds a file
modified in 1950, so the superblock's epoch is negative, and so is every
output built from that tree. A further output, `huge`, puts the small
tree in a sparse volume of 2^32 + 1000 blocks (`-s`), 2 TiB apparent at
512-byte blocks and 16 TiB at 4096, so the block count's bits 32 to 47
are set; no check reads it end to end.

| Oracle | Outputs | Verdict |
| --- | --- | --- |
| `fsck.erofs --extract --preserve` | empty, small, deep, wide, large, every kind, tails, names, mixed, gpt, mbr, huge | Pass at every block size, both settings |
| Kernel loop mount | The same | Pass at every block size, both settings |
| fsimg `W`, `t` | The same | Pass at every block size, both settings |
| rust-fs-erofs 0.5.1 | None | Refuses every image: "feature_incompat 48bit (0x80) block addressing is not implemented by this reader" (first run, then `-x rust`) |

The 8192-byte block refusal passed in every run. Data beyond block 2^32,
which would need more than 2 TiB of stored content, was not built for an
oracle: start blocks above 32 bits are checked by `test-erofs-write`,
which plants a flat and an LZ4 file past block 2^32 in a sparse sink and
reads them back with fsimg's reader, and by E10's `e10-48-large`. A host
source refuses FIFOs, so the implicit link count on a compact FIFO is
checked by the unit tests only.

Image sizes in bytes of the reproducible `-o compression=lz4` plan
(`fsimg c -n -R`) at each addressing:

| Output | `B` | 32-bit | 48-bit |
| --- | --- | --- | --- |
| mixed | 512 | 335872 | 267776 |
| mixed | 4096 | 544768 | 540672 |
| large | 512 | 101666816 | 36700672 |
| large | 4096 | 276901888 | 270610432 |

One 32-byte extent per 64 KiB cluster replaces one 8-byte full-index
record per logical cluster, so at small blocks the 48-bit image is the
smaller.

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
| Compact group packing; the last record and the end marker; inline tail placement; partial references; interlacing; cluster and index limits; LZ4 stream termination; the LZ4 distance field | Packing confirmed, the big-cluster head block following the kernel's walk from the group's anchor; the last record is the marker, the last cluster's own head or plain record inside the last logical cluster, or, at `size mod L = 0`, the last cluster's final record, 592 inodes; the inline tail starts at the index end and lies in one block, not necessarily the inode's; partial references in full indexes only; rotation confirmed; 1 MiB stored and 12 MiB decoded are the kernel's limits, and producers write `L = B` always; blocks need five final literals and a last match 12 bytes before the end; distance 65535 written, 0 accepted. `L > B` and compact advice `0x01` clear are unobservable with the pinned release; E15 observes them hand-built | `e8-*` |
| MicroLZMA, DEFLATE and Zstandard framing, configuration records and limits; mixed head-1 and head-2 | Framings confirmed; DEFLATE window bits at most 15, MicroLZMA dictionary at least 4096 and format 0, Zstandard format 0; head-2 by hand-built images, accepted by the kernel without `COMPR_HEAD2` | `e9-*` |
| Every 48-bit field; encoded extents; writer's 32-byte records | Two superblock forms, the wide one only when a value needs it; start-block high word at `0x06`, `0xFFFF` on an unused extended start; compact bit 4 only for a start above 2^32, ignored by the kernel on an extended inode; omitted dots with `..` first and a synthetic `.` appended by the kernel; chunk bit 6 entries read at block 2^32 + 5; all four encoded-extent widths emitted, the count in the header only for 16 and 32 bytes, derived from the size for 4 and 8; the kernel needs neither `48BIT` for encoded extents nor the count to match the body; the kernel and `fsck.erofs` read 32-byte records around the project's LZ4 blocks in three placements and an LZ4 box ([E10](#e10-48-bit-layout)) | `e10-48-*`, `e10-hand-*`, `e10-edit-*` |
| Kernel acceptance of writer output at 512 to 2048-byte blocks and of the writer's field choices | Accepted: at 512, 1024, 2048 and 4096-byte blocks the writer's output passes the kernel, `fsck.erofs` and rust-fs-erofs, with empty and sub-block flat-inline start words of 0, inodes ending at and moving past block ends, and multi-block directories ([E11](#e11-writer-probes)); and with `compression=lz4` at every block size, LZ4 and plain one-block clusters behind full indexes that cross blocks at 512 bytes, end markers and heads in the last logical cluster included ([LZ4 output](#lz4-output)) | E11, `qualify.sh -b`, `qualify.sh -c lz4` |
| Metadata-box layout, root placement, box compression, shared attributes in the box | The box is a primary-space inode named at `0x80`, flat, or LZ4 behind 4-byte encoded extents, holding every inode but itself and the packed inode, its blocks last; the root is the box's first inode, after the long-prefix table's block when the table is in the box; `METABOX` and `48BIT` are independent and the kernel mounts a hand-patched box volume without `48BIT` rooted in the primary space; the producer keeps the shared pool in the primary space, which Linux 7.0 misreads for some box inodes, and reads a hand-built pool in the box correctly ([E12](#e12-metadata-box)) | `e12-box-*`, `e12-flat-box32-*`, `e12-*-poolbox` |
| Scale: files beyond 4 GiB, a directory of 100000 entries, root NIDs and start blocks at their limits | Files beyond 4 GiB need no 48-bit layout (flat-inline, chunked and LZ4 compact); start blocks reach `0xFFFFFFFE` and NIDs exceed 2^32 without the 48-bit layout; the root NID stays 36 or 128, or bit 63 in a metadata box, so a root NID above 65535 and a nonzero `meta_blkaddr` come only from hand-built images | `e13-*` |
| The fields a volume with external devices sets | Extra device count nonzero in both producer forms, `DEVICE_TABLE`, device identifier 1 in every chunk; `CHUNKED_FILE` only under `--blobdev` | `e14-*` |
| Record count and group layout at `L > B`; compact advice `0x01` clear; a plain inline tail under interlacing | Kernel and both tools read `L = 2B` and `4B`: compact groups laid out for `ceil(size / B)` records, `ceil(size / L)` read, fields `max(lclusterbits, 12)` bits wide, so compact with `L > 4096` needs `0x01` clear, and the inline tail follows record `(size - 1) >> lclusterbits` (full index observed, compact from source); `0x01` clear is all 8-byte groups from the header's end and reads in every tool; no producer writes any of the three, and the design refuses them by policy. A plain inline tail on an interlaced inode has no agreed reading: the kernel reads it only when `E > B - C` with the extent stored rotated left by `B - C`, otherwise copies preceding metadata into the file, and `fsck.erofs` reads other bytes in every case | `e15-*` |
