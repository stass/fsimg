# fsimg

fsimg is a BSD-2-Clause filesystem image tool and embeddable C11 library for
64-bit FreeBSD, Linux, and macOS. Its target design covers FAT and exFAT image
creation, reading, verification, and safe extraction without mounts or root.

The library provides resource contexts and bounded positional I/O. Filesystem
formats and the `fsimg` command are tracked in the
[implementation plan](docs/implementation-plan.md) and [open work](TODO.md).

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
with upstream notices intact. No reference implementation code is bundled.
