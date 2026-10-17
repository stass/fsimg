# Open work

## Major initiatives

* [High] **fsimg tool and library** (`lib/`, `cmd/fsimg/`, `test/`): implement the
  [target specification](specs/README.md) through the ordered
  [implementation plan](docs/implementation-plan.md), continuing with FAT readers
  and structural verification. Format implementation requires platform
  qualification. The acceptance gates cover
  bounded metadata and payload processing, FAT/exFAT interoperability, secure
  extraction, actual-kernel sandbox validation, failure handling, and measured
  operation on gigantic volumes and catalogs.

## Bugs by subsystem

* [Medium] **FreeBSD directory decoding** (`compat/host.c`, `test/source.c`):
  skip unused native directory records with `d_fileno == 0` after validating
  their framing and before validating live names. The decoder treats them as
  entries, and its synthetic live-record fixture leaves the inode zero. On
  FreeBSD, first demonstrate the failure with unused-before-live and unused-only
  batches, then repair the decoder and give live fixtures nonzero inodes.

## Security residuals

* [High] **Platform qualification** (`cmd/fsimg/sandbox.c`, `test/sandbox.c`,
  `.github/workflows/check.yml`): run the confinement prototypes on actual Linux
  and FreeBSD kernels and resolve runtime/libc mismatches before format code
  depends on them. Validate both source and image profiles, failure classification,
  inherited descriptors, alternate ABIs, and publication on deployment
  filesystems. Integrate bounded directory reads and descriptor-only mount
  queries with the source profile: Linux seccomp denies the scanner's `statx`,
  FreeBSD needs mount-query rights, and confined source roots need private
  descriptor adoption. Do not permit general pathname metadata queries or
  descriptor duplication to make these paths work. Qualify rejection of
  same-filesystem mount aliases with real mount fixtures. The macOS host cannot
  establish those kernel guarantees; see
  [platform evidence](docs/platform-evidence.md).

## Planned features

* [Medium] **Additional inputs and formats** (`lib/source/`, `lib/formats/`):
  design manifest import, bounded partition selection, and independent NTFS,
  QNX4, and QNX6 reader profiles as concrete needs arise. Follow the
  [extension sequence](docs/implementation-plan.md#extension-sequence); each
  format needs a bounded-resource design and an independent compatibility corpus.
