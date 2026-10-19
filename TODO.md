# Open work

## Major initiatives

* [High] **QNX6 read/write and automotive qualification**
  (`lib/formats/qnx6/`, `lib/read/`, `lib/write/`): implement the
  [Power-Safe design](specs/design-qnx6.md), close the explicit format-evidence
  gates, and qualify native read/write interoperability across classic,
  automotive, and extended variants. Porsche PCM 3.1 HDD reproduction also requires
  metadata fidelity and exact disk composition, including QNX4 where present.
  The [work breakdown](docs/todo-qnx6.md) owns experiments and implementation;
  [research evidence](docs/qnx6-research.md) owns source provenance.

* [Medium] **Architecture consolidation** (`lib/read/`, `lib/write/`,
  `lib/formats/`): move traversal, namespace validation, and content
  delivery to the single owners in the
  [consolidation plan](docs/implementation-plan.md#architecture-consolidation).
  The drivers still implement their own traversal engines and content loops.

## Bugs by subsystem

* [Low] **Source failures fail the writer from inside the source layer**
  (`lib/source/source.c`, `specs/design-sources.md`): an operational source
  failure calls `fsimg_writer_fail()` and releases collection storage while a
  driver's emit call is still on the stack, whereas a sink failure during the
  same copy fails the writer only when `fsimg_writer_emit()` returns. Make the
  source cursor report status only and leave failing the writer to the writer
  lifecycle, so writer state after a failed copy no longer depends on which
  layer failed.
* [Low] **Monotonic-clock failures** (`cmd/fsimg/paths.c`,
  `cmd/fsimg/worker-parent.c`, `cmd/fsimg/main.c`): make later supervision,
  publication and reporting clock queries distinguish failure from zero.
  Initial deadline setup already returns `FSIMG_EIO`; later helpers can derive
  a reporting deadline or defer timeout detection after a failed query.
  Centralize the clock result and exercise failures without weakening bounded
  shutdown or adding a blocking diagnostic path.

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

* [Medium] **Scratch descriptors grow with directory depth** (`lib/meta/store.c`,
  `lib/formats/fat/directory.c`, `lib/formats/exfat/directory.c`): each spilled
  ancestor directory stream holds its own scratch descriptor. A 24-level tree
  with one cache page peaks at 30 FAT reader descriptors and fails at
  `max_open_files=16`. Extrapolating, depth near 120 can exhaust the default
  128 descriptors although `max_depth` is 256. The
  [prepared-directory design](specs/design-directories.md#traversal) requires
  shared scratch for retained ancestors; resolve it with the common walker.

## Security residuals

## Planned features

* [Medium] **Additional inputs and formats** (`lib/source/`, `lib/formats/`):
  design manifest import, bounded partition selection, and independent NTFS and
  QNX4 reader profiles as concrete needs arise. Follow the
  [extension sequence](docs/implementation-plan.md#extension-sequence); each
  format needs a bounded-resource design and an independent compatibility corpus.
