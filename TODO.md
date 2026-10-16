# Open work

## Major initiatives

* [High] **fsimg tool and library** (`lib/`, `cmd/fsimg/`, `test/`): implement the
  [target specification](specs/README.md) through the ordered
  [implementation plan](docs/implementation-plan.md), continuing with source
  scanning and the deterministic catalog. Format implementation requires platform
  qualification. The acceptance gates cover
  bounded metadata and payload processing, FAT/exFAT interoperability, secure
  extraction, actual-kernel sandbox validation, failure handling, and measured
  operation on gigantic volumes and catalogs.

## Security residuals

* [High] **Platform qualification** (`cmd/fsimg/sandbox.c`, `test/sandbox.c`,
  `.github/workflows/check.yml`): run the confinement prototypes on actual Linux
  and FreeBSD kernels and resolve runtime/libc mismatches before format code
  depends on them. Validate both source and image profiles, failure classification,
  inherited descriptors, alternate ABIs, and publication on deployment
  filesystems. The macOS host cannot establish those kernel guarantees; see
  [platform evidence](docs/platform-evidence.md).

## Planned features

* [Medium] **Additional inputs and formats** (`lib/source/`, `lib/formats/`):
  design manifest import, bounded partition selection, and independent NTFS,
  QNX4, and QNX6 reader profiles as concrete needs arise. Follow the
  [extension sequence](docs/implementation-plan.md#extension-sequence); each
  format needs a bounded-resource design and an independent compatibility corpus.
