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
2 when the tree is not decoded (the 48-bit layout and the metadata box).

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
