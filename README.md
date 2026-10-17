# fsimg

fsimg is a BSD-2-Clause filesystem image tool and embeddable C11 library for
64-bit FreeBSD, Linux, and macOS. Its target design covers FAT and exFAT image
creation, reading, verification, and safe extraction without mounts or root.

The library provides resource contexts, bounded positional I/O, and private
bounded metadata storage with record streams, paged arrays/bitsets, and stable
external sorting. Private writer infrastructure scans host trees, reads zero
and provider-backed sources, and prepares deterministic catalogs without a
resident object or descriptor per entry. Source scanning rejects symlinks,
special files, descendant mounts, and scratch-directory overlap; identity checks
detect ordinary changes, while hostile concurrent mutation requires immutable
sources or a snapshot. Public FAT12/FAT16/FAT32 readers enumerate entries,
read current-file content, report volume information, and verify complete
allocation ownership with optional payload read-through. Validation targets
macOS; Linux/FreeBSD qualification remains open. FAT writers and the `fsimg` command are
tracked in the [implementation plan](docs/implementation-plan.md) and
[open work](TODO.md).

Full FAT verification rejects dirty/error flags and inconsistent known FSInfo
free-count hints, as well as structural corruption; it performs no repair.
Supply a private scratch directory for large directory, ownership, and fragment
indexes. Without scratch, operations that exceed resident metadata storage
return a resource-limit error.

Build with Autoconf, Automake, Libtool, a C11 compiler, and a C++ compiler:

```sh
autoreconf -fi
mkdir build
cd build
../configure
make
make check
```

See [build and validation](docs/build.md),
[platform evidence](docs/platform-evidence.md), and the
[specifications](specs/README.md). The ordinary library requires only libc and
host OS APIs. Development tests run unprivileged in disposable fixtures.

Project-owned files carry SPDX BSD-2-Clause notices and contributor copyright;
[LICENSE](LICENSE) contains the terms. Autotools generates its own build support
with upstream notices intact. FAT codec examples adapted from the author's
libarchive reader retain their BSD-2-Clause notice. Pinned Unicode/CP437 data
uses the Unicode-3.0 license. The [reference manifest](docs/references.md) records
provenance; installed documentation includes the required notices.
