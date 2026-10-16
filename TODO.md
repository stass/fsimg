# Open work

## Major initiatives

* [High] **fsimg tool and library** (`lib/`, `cmd/fsimg/`, `test/`): implement the
  [target specification](specs/README.md) through the ordered
  [implementation plan](docs/implementation-plan.md). The acceptance gates cover
  bounded metadata and payload processing, FAT/exFAT interoperability, secure
  extraction, actual-kernel sandbox validation, failure handling, and measured
  operation on gigantic volumes and catalogs.

## Planned features

* [Medium] **Additional inputs and formats** (`lib/source/`, `lib/formats/`):
  design manifest import, bounded partition selection, and independent NTFS,
  QNX4, and QNX6 reader profiles as concrete needs arise. Follow the
  [extension sequence](docs/implementation-plan.md#extension-sequence); each
  format needs a bounded-resource design and an independent compatibility corpus.
