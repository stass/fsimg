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
detect changes visible in host metadata; coarse timestamps and metadata-preserving
mutation require immutable sources or a snapshot for consistency. Public
FAT12/FAT16/FAT32 readers enumerate entries,
read current-file content, report volume information, and verify complete
allocation ownership with optional payload read-through. Native and sanitizer
suites run on macOS, Linux, and FreeBSD, with platform/tool skips documented
in the evidence. Public FAT12/FAT16/FAT32 writers
plan deterministic layouts, stream payloads directly from stable sources, and
emit to bounded sinks or publish a new image without replacing an existing
name. Plans expose geometry, space use, and metadata-loss counts. Safe extraction
creates a fresh private tree with logical quotas, case/alias selection, and
optional timestamp restoration. Private executable modules supervise confined
workers and bounded stdout/stderr output; Linux/FreeBSD profiles have actual
kernel tests. The `fsimg`
command and remaining work are tracked in the
[implementation plan](docs/implementation-plan.md) and
[open work](TODO.md).

Full FAT verification rejects dirty/error flags and inconsistent known FSInfo
free-count hints, as well as structural corruption; it performs no repair.
Supply a private scratch directory for large directory, ownership, and fragment
indexes. Without scratch, operations that exceed resident metadata storage
return a resource-limit error.

Writers support 512 through 4096-byte sectors and allocation units through
32 KiB. Automatic FAT sizing follows Microsoft's 16-cluster margin around
variant cutovers; exact sizing retains the legal boundary counts. Required
metadata loss fails; optional timestamp rounding, range and
timezone-basis losses follow the initialized options' explicit allow-loss
policy and optional callback. Reproducible mode selects a fixed UTC epoch.
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
