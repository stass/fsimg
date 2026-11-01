# fsimg

Create, inspect, verify, and extract filesystem images without mounting them.

fsimg brings a tar-style interface to disk images: list a volume, read a file
to stdout, extract a directory, or build a new image from a source tree.
It runs on 64-bit Linux, FreeBSD, and macOS, with no root privileges, kernel
filesystem driver, or external filesystem utilities required for image files.

```sh
fsimg tf disk.img                         # List files
fsimg xOf disk.img /etc/motd               # Read one file to stdout
fsimg xf disk.img                         # Extract into ./disk/
fsimg cf data.img -F exfat -C files .      # Create an image from ./files/
```

## Features

- **One interface across formats.** Work with FAT, exFAT, and QNX filesystems
  using the same commands.
- **Read through the layers.** Detect supported filesystems inside GPT or MBR
  disks, DMGs, fixed VHDs, Android sparse images, and gzip, zstd or xz
  streams.
- **Build images from files or manifests.** Choose the filesystem, size,
  label, partition scheme, and container. Use mtree manifests to set image
  ownership and permissions without changing host files.
- **Plan and reproduce builds.** Inspect sizing and metadata losses before
  writing; produce reproducible images with `-R`; shrink an image to the
  smallest geometry its files fit in with `-M`; verify staged images before
  publishing them with `cW`.
- **Security and sandboxing.** Process untrusted images in an OS sandbox
  where supported, with explicit warnings when confinement is unavailable.
  Use `-S required` to refuse unconfined operation, and set memory, scratch,
  output, and work limits.
- **Extract into a fresh tree.** Extraction never merges with existing files
  or overwrites them.
- **Use it from scripts or C.** Send a file's bytes to stdout, consume streaming
  JSON records, or embed the same readers and writers through `libfsimg`.

## Quick start

[Build and install fsimg](#build), then try a complete round trip with a tiny
image. No download or existing disk image is needed:

```sh
demo=$(mktemp -d)
cd "$demo"
mkdir source
printf 'hello from fsimg\n' > source/hello.txt

# Create an exFAT image, sized automatically to fit.
fsimg cf hello.img -F exfat -C source .

# Browse it and read a file without extracting it.
fsimg tf hello.img
fsimg xOf hello.img /hello.txt

# Check its structure and read all file payloads.
fsimg Wdf hello.img

# Extract into a new ./hello/ directory and compare the result.
fsimg xf hello.img
cmp source/hello.txt hello/hello.txt
```

The read command prints `hello from fsimg`; `cmp` succeeds silently.
The files remain in the temporary directory for you to explore.

The command letters follow tar: `c` creates, `t` lists, `x` extracts,
`i` reports image information, and `W` verifies. `-f` names the image.
Use `fsimg -h` for an overview or `fsimg c -h` for help with a command.

## Everyday use

### Inspect a disk or compressed image

```sh
fsimg if disk.img                         # Layers, partitions, volume details
fsimg tvf disk.img                        # Long listing
fsimg tf disk.img --partition=2           # Select a partition explicitly
fsimg tf disk.img --partition=name=EFI /EFI
fsimg tf disk.img.gz                      # Gzip is detected automatically
fsimg tf disk.dmg                         # Container and volume are detected
```

Without an explicit selection, fsimg uses the one partition containing a
recognized filesystem. `fsimg i` shows the partition table even when you need
to choose a partition. Readable disk devices work too, subject to device
permissions: for example, `fsimg if /dev/disk10` on macOS. Keep the medium
stable while reading it.

### Extract just what you need

```sh
fsimg xf disk.img /documents              # Creates ./disk/documents/
fsimg xOf disk.img /documents/readme.txt > readme.txt
```

By default, extraction creates a private directory named after the image:
`disk.img`, `disk.img.gz`, and `disk.dmg` all extract into `./disk/`.
That directory must not already exist. To use an existing empty private
directory, pass `-E -C directory`.

Extraction creates regular files and directories. For a QNX image containing
symbolic links or FIFOs, use `-k` to skip those entries; add `--mtree` to record
them and their metadata for a later rebuild:

```sh
fsimg xkf rootfs.img --mtree=rootfs.mtree
fsimg cf rebuilt.img -F qnx6 -C rootfs @../rootfs.mtree
```

The manifest carries ownership, modes, link targets, and file digests that the
extracted tree alone cannot preserve. This rebuilds the tree and supported
metadata; it does not reproduce the original disk layout or bootability.

### Create a volume, disk, or container

With a source directory `files/`, choose the image you need:

```sh
# A FAT32 volume with a label and an exact size.
fsimg cf data.img -F fat32 -s 64M -L DATA -C files .

# A reproducible exFAT image, verified before publication.
fsimg cWRf data-exfat.img -F exfat -C files .

# The smallest FAT16 volume the files fit in.
fsimg cMf data-small.img -F fat16 -C files .

# A FAT32 volume inside a GPT disk, then inside a compressed DMG.
fsimg cf data.dmg -F fat32/gpt/dmg -C files .

# A gzip-compressed image.
fsimg czf data.img.gz -F exfat -C files .

# Inspect a plan without creating an image or reading file payloads.
fsimg cn -F exfat --free-space=32M -C files .
```

`-F` names layers from the inside out: `fat32/gpt/dmg` means a FAT32
filesystem in a GPT partition inside a DMG. Creation writes one partition;
the C API can compose several volumes into a disk. Images are sized
automatically unless you request an exact size with `-s`.

By default, fsimg picks the first legal geometry for the size it needs,
leaving the room a native formatter would. `-M` instead tries every open
geometry setting and keeps the one giving the smallest volume: every
cluster size for FAT and exFAT, and every block size for QNX6, with an inode
table that just holds the entries. Settings you give with `-o`, such as
`cluster=` or `inodes=`, and `--free-space` still apply, and the output is as
reproducible as any other build. `-M` conflicts with `-s`. Check the result
first with `fsimg cnM ...`, which prints the chosen cluster or block size.

A minimized image has little spare room for metadata. On FAT12 and FAT16
the root directory holds only the entries it was created with, up to a
whole sector, so a system that later adds files to the root soon finds it
full; `-o root=N` gives the root N entries, rounded up to a whole sector,
so it keeps room for more. QNX6 has almost no free inodes unless you pass
`inodes=`. A small tree may already be at the format's minimum, such as
FAT32's 65,525 clusters or exFAT's 1 MiB, and then `-M` gives the same
image.

Creation publishes a new file and refuses to replace an existing name.
Keep source files stable during the operation. `-R` fixes ordering,
timestamps, identifiers, and padding; compressed bytes also depend on the
zlib version.

### Use pipelines and structured output

```sh
# Read a compressed image from a pipe.
cat disk.img.gz | fsimg tf -

# Write a gzip-compressed image to stdout.
fsimg cRzf - -F exfat -C files . > streamed.img.gz

# Stream a machine-readable listing.
fsimg tf disk.img --json
```

JSON output is newline-delimited, with explicit completion records. Diagnostics
and progress go to stderr; file payloads and listing records go to stdout.
Standard input and decompression use a temporary spool, bounded by
`-l spool=` (64 GiB by default). Use `-T` to choose scratch storage and
`-m` to set the library memory budget.

## Supported formats

| Filesystem | Read, create, verify, extract | Scope |
| --- | --- | --- |
| FAT12, FAT16, FAT32 | Yes | Long names and FAT short aliases |
| exFAT | Yes | Ordinary one-FAT volumes |
| QNX6 Power-Safe | Yes | Classic QNX 6.5.0 layout, either byte order |
| QNX4 | Yes | QNX 6.5.0 `dinit` layout, including long names |
| QNX ImageFS (IFS) | Yes | Either byte order; newly created bodies are nonbootable |

| Disk layer | Read | Create |
| --- | --- | --- |
| GPT, MBR | Yes | One partition through the CLI; multiple through the C API |
| DMG (UDIF) | Stored, zero, and zlib chunks | UDRO or UDZO |
| VHD | Fixed disks | Fixed disks |
| Android sparse (`simg`) | Yes | Yes |
| gzip | Files and stdin, including concatenated members | Files and stdout |
| zstd (with libzstd) | Files and stdin, including concatenated and skippable frames | Files and stdout |
| xz (with liblzma) | Files and stdin, including concatenated streams | Files and stdout |
| QNX boot image (`ifsboot`) | Raw, LZO1X, or NRV2B body | Qualified machine/compression combinations |
| Harman-Becker `hbcifs` | LZO1X ImageFS container | No |

Run `fsimg --formats` for the capabilities in your build. Container support
does not add support for the filesystem inside it: an HFS+ or APFS DMG, for
example, cannot be listed or extracted.

fsimg creates new images and reads existing ones; it does not edit or repair
them in place. QNX support targets the layouts above, rather than every
Power-Safe or firmware variant. ImageFS boot-image creation has additional
restrictions, and `hbcifs` verification reports incomplete coverage.
See the [command manual](docs/fsimg.1) for exact profiles and limits.

## Build

Building from a checkout requires Autoconf, Automake, Libtool, make, a C11
compiler, a C++ compiler, and the system zlib development headers and library.
The zstd and xz compressions use libzstd and liblzma when configure finds
them (`libzstd-dev` and `liblzma-dev`, `libzstd-devel` and `xz-devel`,
Homebrew's `zstd` and `xz`; on FreeBSD the `archivers/zstd` port and the
base system's liblzma); without one the build is otherwise the same and
refuses to read or write that compression.

```sh
git clone https://github.com/stass/fsimg.git
cd fsimg
autoreconf -fi
mkdir build
cd build
../configure --prefix="$HOME/.local"
make
make install
export PATH="$HOME/.local/bin:$PATH"
```

From a release archive, start with `./configure`; generated build support is
included. Ruby 3.2 or later, Minitest and pkg-config are needed for the CLI
integration and installation checks. See [build and validation](docs/build.md)
for platform setup and test commands.

Installation includes the command, shared and static `libfsimg`, the public C
header, pkg-config metadata, manuals, and checked
[C examples](docs/examples.md). The runtime uses libc, host OS APIs, system
zlib and, when the build found it, the system liblzma.

## Further reading

After installation, read the manuals with `man fsimg` and `man libfsimg`.

- [Command manual](docs/fsimg.1): options, extraction rules, format profiles,
  resource limits, and failure behavior.
- [Library manual](docs/libfsimg.3) and [C examples](docs/examples.md): reading,
  creating, extracting, and supplying callback-backed file content.
- [Validation and qualification](docs/qualification.md) and
  [platform evidence](docs/platform-evidence.md): interoperability, scale,
  fuzzing, and platform checks.
- [Specifications](specs/README.md) and [open work](TODO.md): design and
  remaining work.

The CLI uses OS confinement where available; `-S required` refuses to run
without it. Standalone macOS has no built-in confinement backend.
Verification checks supported structures and checksums; it is not a repair
operation. Failed extraction can leave an explicitly incomplete private tree.
See the command manual for recovery and publication outcomes.

## License

[BSD-2-Clause](LICENSE). Third-party code and data retain their own notices;
see the [reference manifest](docs/references.md).
