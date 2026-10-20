# fsimg

fsimg is a BSD-2-Clause filesystem image tool and embeddable C11 library for
64-bit FreeBSD, Linux, and macOS. It supports FAT and exFAT image
creation, reading, verification, and safe extraction without mounts or root,
and reads and verifies classic QNX6 Power-Safe volumes.

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
optional timestamp restoration. The `fsimg` command provides `create`, `info`,
`ls`, `cat`, `extract`, `verify`, and `formats`, with streaming NDJSON output
and bounded progress. It supervises confined workers and bounded stdout/stderr
output; Linux/FreeBSD profiles have actual kernel tests. The
[implementation plan](docs/implementation-plan.md) and [open work](TODO.md)
record qualification gates and remaining work.

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
planning, and owner and mode imported with `create -p` are counted optional
losses that leave the image bytes unchanged. Extraction never creates links
or FIFOs; `extract -k` skips and counts such entries instead of failing.

The QNX6 reader accepts the classic QNX 6.5.0 layout in either byte order at
512 through 4096-byte blocks, selects the newer of the two snapshot headers,
and reports files, directories, symbolic links and FIFOs with owner, mode and
all four timestamps. It finds the second header from the slice length, so a
partition is read with `-b` and `-z` set to its exact offset and length.
Hard links appear as separate entries with the same content, and names are
compared byte for byte. QNX6 names reach 510 bytes; extracting one longer
than the host filesystem allows, 255 bytes on most, fails as unsupported.
Bootless and Audi MMI layouts and later Power-Safe extensions are recognized
and refused as unsupported; QNX4 is not recognized. Writing QNX6 awaits
native qualification.
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
See `fsimg -h`, `fsimg create -h`, and `man fsimg` for the command contract.
Creation and extraction are quiet on success. Use `-n` to inspect a creation
plan, `-v` for progress and sampled metadata-loss details, or `-j` for structured
records with explicit completion. Human errors report relevant paths and OS
messages; metadata-loss notices count affected fields rather than files.
Sandbox-unavailable warnings remain visible. Standalone macOS has no built-in
OS confinement backend; `-S required` refuses operation there.

```sh
fsimg formats
fsimg create -R -t exfat -o new.img source-directory
fsimg verify -d -i new.img
fsimg ls -j -r -i new.img
fsimg extract -i new.img -C new-directory
```

See [build and validation](docs/build.md),
[platform evidence](docs/platform-evidence.md), and the
[specifications](specs/README.md). The ordinary library requires only libc and
host OS APIs. Development tests run unprivileged in disposable fixtures.

Project-owned files carry SPDX BSD-2-Clause notices and contributor copyright;
[LICENSE](LICENSE) contains the terms. Autotools generates its own build support
with upstream notices intact. FAT codec examples adapted from the author's
libarchive reader and writer retain their BSD-2-Clause notices. Pinned Unicode/CP437 data
uses the Unicode-3.0 license. The [reference manifest](docs/references.md) records
provenance; installed documentation includes the required notices.
