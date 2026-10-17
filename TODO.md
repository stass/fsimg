# Open work

## Major initiatives

* [High] **fsimg tool and library** (`lib/`, `cmd/fsimg/`, `test/`): implement the
  [target specification](specs/README.md) through the ordered
  [implementation plan](docs/implementation-plan.md). Continue with the CLI
  and release qualification. The acceptance gates cover
  bounded metadata and payload processing, FAT/exFAT interoperability, secure
  extraction, actual-kernel sandbox validation, failure handling, and measured
  operation on gigantic volumes and catalogs.

## Bugs by subsystem

* [Medium] **UFS publication handoff** (`lib/output/publish.c`,
  `test/publish.c --identity`, `test/extract.c`): investigate deferred ctime
  changes on FreeBSD 15.1/UFS after publication. Immediate readers can reject
  unchanged payloads with `FSIMG_ESOURCE_CHANGED`; inode fsync and an atomic
  no-replace-rename prototype do not establish a stable metadata baseline.
  Retain conservative identity checks and qualify the kernel bookkeeping before
  promising immediate publication-to-reader handoff on this filesystem.

* [Low] **Source-scanner diagnostics** (`lib/source/scan.c`, `compat/host.c`,
  `test/source.c`): a socket that fails during open is reported as host I/O
  failure instead of unsupported source kind. Add a socket regression and
  classify the failure without guessing object type from `ENXIO` alone.

* [Low] **FAT volume-label escape** (`lib/formats/fat/codec.c`,
  `lib/formats/fat/directory.c`, `test/fat.c`): verify the directory-name
  first-byte `0x05` escape for a CP437 label beginning with `0xe5`, separately
  from the BPB label field. Add a failing root/BPB label fixture before
  changing the decoder's interpretation of either field.

## Security residuals

## Planned features

* [Medium] **Additional inputs and formats** (`lib/source/`, `lib/formats/`):
  design manifest import, bounded partition selection, and independent NTFS,
  QNX4, and QNX6 reader profiles as concrete needs arise. Follow the
  [extension sequence](docs/implementation-plan.md#extension-sequence); each
  format needs a bounded-resource design and an independent compatibility corpus.
