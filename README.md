# fsimg

fsimg is a BSD-2-Clause filesystem image tool and embeddable C11 library for
64-bit FreeBSD, Linux, and macOS. It supports FAT, exFAT, classic QNX6
Power-Safe and QNX4 image creation, reading, verification, and safe
extraction without mounts or root.

The library provides resource contexts, bounded positional I/O, and private
bounded metadata storage with record streams, paged arrays/bitsets, and stable
external sorting. Private writer infrastructure scans host trees, reads zero
and provider-backed sources, and prepares deterministic catalogs without a
resident object or descriptor per entry. Source scanning records symbolic
links by target without following them and rejects special files, descendant
mounts, and scratch-directory overlap; identity checks
detect changes visible in host metadata; coarse timestamps and metadata-preserving
mutation require immutable sources or a snapshot for consistency. Public
FAT12/FAT16/FAT32/exFAT readers enumerate entries,
read current-file content, report volume information, and verify complete
allocation ownership with optional payload read-through. Native and sanitizer
suites run on macOS, Linux, and FreeBSD, with platform/tool skips documented
in the evidence. Public FAT12/FAT16/FAT32/exFAT writers
plan deterministic layouts, stream payloads directly from stable sources, and
emit to bounded sinks or publish a new image without replacing an existing
name. Plans expose geometry, space use, and metadata-loss counts. Safe extraction
creates a fresh private tree with logical quotas, case/alias selection, and
optional timestamp restoration. The tar-like `fsimg` command creates (`c`), lists (`t`),
extracts (`x`, or `x -O` to stdout), verifies (`W`) and describes (`i`)
images and reports `--formats`, with streaming NDJSON output
and bounded progress. It supervises confined workers and bounded stdout/stderr
output; Linux/FreeBSD profiles have actual kernel tests. The
[implementation plan](docs/implementation-plan.md) and [open work](TODO.md)
record qualification gates and remaining work.

Reading a disk peels its layers: `-F` names them inner to outer
(`fat32/gpt`), and an unnamed slot is detected. GPT and MBR partition tables
are read and checked completely before any partition is used: both GPT
copies must be intact and agree, entries must lie within the usable range
without overlapping, and a damaged table fails rather than falling back to
the disk as a bare volume. The volume is the one partition holding a
recognized filesystem unless `--partition=N`, `name=TEXT` or `type=NAME`
selects one; `--offset=`/`--length=` bypass the table. `fsimg i` lists the
table and marks the selected partition, and `fsimg W` checks the table with
the volume.

DMG, fixed VHD and Android sparse image containers are read the same way:
`fsimg xf installer.dmg` unwraps the DMG, the GPT inside it and the volume
inside that. DMG chunks may be stored, zero or zlib-compressed, as `hdiutil`
writes UDRO and UDZO images; the koly trailer, the property list and every
block table are validated before any chunk is used, and decoded chunks are
cached four at a time within the memory limit, the decoding counted as
work. `fsimg W` checks the container's checksums, and `W -d` decodes every
chunk. Other DMG compressions, multi-segment DMGs and dynamic VHDs are
refused as unsupported.

A gzip-compressed image is read too, as a file or on standard input:
`fsimg tf sd.img.gz`, or `curl -s URL | fsimg xf - -C out`. The decoder
checks every member's CRC32 and length and refuses trailing bytes; it
writes the image into the spool, an unlinked file in scratch whose logical
size `-l spool=` bounds (64 GiB by default) before each step, so a
decompression bomb stops at the limit. An uncompressed image on standard
input is copied there unchanged. When gzip's signature and another format
both claim the first bytes, `-z` or `-F` says which reading is meant.

Creating with a scheme in the stack writes a disk: `fsimg cf disk.img -F
fat32/gpt -C esp .` puts the volume at 1 MiB as the disk's one partition,
typed and named after its filesystem unless `-o gpt:type=`, `gpt:name=`,
`mbr:type=` or `mbr:active=` say otherwise, with the disk length derived or
fixed by `gpt:disk=`/`mbr:disk=` and 4096-byte GPT sectors on request.
Identifiers are random, or under `-R` derived from the epoch. The library
also wraps a raw volume in a scheme, unwraps a partition back to its raw
bytes, and composes several volumes into one disk.

A container in the stack is written too: `fsimg cf disk.dmg -F fat32/gpt/dmg
-C esp .` writes a UDZO DMG that `hdiutil` verifies and attaches, and
`fat32/vhd` and `fat32/gpt/simg` write a fixed VHD and an Android sparse
image. The disk is emitted to a private raw file and encoded in one
sequential pass, all-zero chunks becoming zero runs; `-o dmg:compress=`,
`dmg:level=`, `dmg:chunk=`, `vhd:align=`, `simg:block=` and `simg:crc=` tune
it. `fsimg cf - ...` streams the image to standard output instead, refusing
a terminal. Writing compression is not implemented.

[System qualification](docs/qualification.md) includes ten-million-file format
planning, a million-fragment content round trip, sustained fuzzing and combined
CLI memory checks. The extended targets run separately from ordinary tests.

Full FAT verification rejects dirty/error flags and inconsistent known FSInfo
free-count hints, as well as structural corruption; it performs no repair.
Supply a private scratch directory for large directory, ownership, and fragment
indexes. Without scratch, operations that exceed resident metadata storage
return a resource-limit error.

Writers support 512 through 4096-byte sectors. FAT allocation units reach
32 KiB and exFAT units reach 32 MiB, while transfer buffers remain bounded
independently. Actual sparse 128 TiB exFAT volumes are qualified at a 64 MiB
library budget on APFS and ZFS. exFAT validates bitmap allocation, embedded
case mappings, and directory entry sets; initialized-data tails always read
as zero. Its timestamps retain per-field UTC, known-offset or wall-clock bases.
Automatic FAT sizing follows Microsoft's 16-cluster margin around
variant cutovers; exact sizing retains the legal boundary counts. Required
metadata loss fails; optional timestamp rounding, range and
timezone-basis losses follow the initialized options' explicit allow-loss
policy and optional callback. Reproducible mode selects a fixed UTC epoch.
The entry model also carries symbolic links, FIFOs, owner, mode and change
time. FAT and exFAT store none of them: a symbolic link or FIFO fails
planning, and owner and mode imported with `fsimg c -p` are counted optional
losses that leave the image bytes unchanged. Extraction never creates links
or FIFOs; `fsimg x -k` skips and counts such entries instead of failing.

The QNX6 reader accepts the classic QNX 6.5.0 layout in either byte order at
512 through 4096-byte blocks, selects the newer of the two snapshot headers,
and reports files, directories, symbolic links and FIFOs with owner, mode and
all four timestamps. It finds the second header from the slice length, so a
partition is read through its partition table or with `--offset=` and
`--length=` set to its exact offset and length. Hard links appear as separate entries with the same content, and names are
compared byte for byte. QNX6 names reach 510 bytes; extracting one longer
than the host filesystem allows, 255 bytes on most, fails as unsupported.
Bootless and Audi MMI layouts and later Power-Safe extensions are recognized
and refused as unsupported. The QNX6 writer creates
the same layout with `-o endian=`, `uuid=`, `inodes=` and `reserve=`
settings and stores every entry kind and the POSIX fields; QNX 6.5.0 checks,
mounts, modifies and remounts its volumes cleanly at every block size and in
both byte orders.

The QNX4 reader accepts the volumes QNX 6.5.0 `dinit` makes, with or without
`.longfilenames`, and reports files, directories, symbolic links and FIFOs
with owner, mode and all four timestamps, and the label of up to 48 bytes.
It derives the block count from the slice length, so a partition is read
through its partition table or with `--offset=` and `--length=` set to its
exact offset and length. Names
reach 505 bytes
on a volume with `.longfilenames` and 48 bytes without; they compare byte for
byte, and hard links appear as separate entries with the same content. The
format's own `.bitmap`, `.inodes`, `.longfilenames` and `lost+found` are not
listed; `.boot` and `.altboot` are. `fsimg W` checks that every allocated
block has exactly one owner, that every link count matches the names, and
that every used `.inodes` slot and long-name record is named; it reports a
volume whose label is invalid or
whose `lost+found` holds an entry as unsupported, since neither has an
observed form. A volume whose `lost+found` `..` names the root record does
not open, for the same reason. The QNX4 writer creates the layout QNX
6.5.0 `dinit` makes, with `.longfilenames`, names of up to 505 bytes, a
label of up to 48 bytes and an `-o inodes=` setting for the `.inodes`
capacity. Every object is one extent, and the loader block is zero, so the
volume is not bootable. It stores every entry kind and the POSIX fields.
QNX 6.5.0 checks, mounts and remounts its volumes cleanly at every size
from the 43-block minimum to a 52.6 GB volume, and modifies every one with
free space cleanly.
FAT stores calendar fields without a timezone, and readers report wall times.
Raw sinks may be incomplete on failure. Callback source and sink storage must
be disjoint. File publication can report an existing target, uncertain outcome,
or durability failure; inspect the structured error before retrying. Unsupported
publication retains the private completed image and reports its location.
Failed extraction retains an explicitly incomplete private tree. FreeBSD/UFS
immediate publication-to-reader handoff can fail conservative identity checks;
its ctime behavior remains under investigation.

Build with Autoconf, Automake, Libtool, a C11 compiler, and a C++ compiler:

```sh
autoreconf -fi
mkdir build
cd build
../configure
make
make check
make check-install
```

For a release archive, start with `./configure`; generated build support is
included. `make install` installs the command, static/shared library, public
header, pkg-config metadata, manuals and checked [C examples](docs/examples.md).
The command is shaped like tar: a mode letter (`c` create, `x` extract, `t`
list, `W` verify, `i` information) followed by option letters, with `-f`
naming the image. See `fsimg -h`, `fsimg c -h`, and `man fsimg` for the
command contract. Creation and extraction are quiet on success. Use `-n` to
inspect a creation plan, `-vv` for progress and sampled metadata-loss details,
or `--json` for structured records with explicit completion. Human errors report relevant paths and OS
messages; metadata-loss notices count affected fields rather than files.
Sandbox-unavailable warnings remain visible. Standalone macOS has no built-in
OS confinement backend; `-S required` refuses operation there.

```sh
fsimg --formats
fsimg cRf new.img -F exfat -C source-directory .
fsimg Wdf new.img
fsimg tf new.img --json
fsimg xf new.img
```

See [build and validation](docs/build.md),
[platform evidence](docs/platform-evidence.md), and the
[specifications](specs/README.md). The ordinary library requires only libc,
host OS APIs and the system zlib, which DMG chunks need and which is never
vendored. Development tests run unprivileged in disposable fixtures.

Project-owned files carry SPDX BSD-2-Clause notices and contributor copyright;
[LICENSE](LICENSE) contains the terms. Autotools generates its own build support
with upstream notices intact. FAT codec examples adapted from the author's
libarchive reader and writer retain their BSD-2-Clause notices. Pinned Unicode/CP437 data
uses the Unicode-3.0 license. The [reference manifest](docs/references.md) records
provenance; installed documentation includes the required notices.
