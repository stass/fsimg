# Open work

## Major initiatives

* [High] **QNX6 read/write and automotive qualification**
  (`lib/formats/qnx6/`, `lib/read/`, `lib/write/`): implement the
  [Power-Safe design](specs/design-qnx6.md) through its
  [driver design](specs/design-qnx6-driver.md) and the
  [POSIX metadata design](specs/design-metadata.md), close the explicit
  format-evidence
  gates, and qualify native read/write interoperability across classic,
  automotive, and extended variants. Porsche PCM 3.1 HDD reproduction also requires
  metadata fidelity and exact disk composition, including QNX4 where present.
  The [work breakdown](docs/todo-qnx6.md) owns experiments and implementation;
  [research evidence](docs/qnx6-research.md) owns source provenance.

* [Low] **Scoped ownership intervals** (`lib/formats/fat/verify.c`,
  `lib/formats/exfat/verify.c`): extract the duplicate ownership-interval
  sorting and cursor checks into one bounded utility, the last item of the
  [consolidation plan](docs/implementation-plan.md#architecture-consolidation).
  Both verifiers keep their format-specific reconciliation.

## Bugs by subsystem

* [Medium] **Provider symbolic-link targets are not checked for NUL bytes**
  (`lib/source/source.c`, `specs/design-metadata.md`): the metadata design
  says a link target never contains a NUL byte. Host targets cannot, and the
  catalog rejects the zero source for links, but a provider target reaches a
  driver unchecked. Before the first writer that stores links, read a
  provider link's target into the cursor's bounded `link_target` buffer when
  `fsimg_source_open()` opens it, reject a NUL byte there, and serve the
  buffer, so every link target is checked once in the common layer.

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

## Security residuals

* [High] **Symbolic-link import under the Linux sandbox** (`cmd/fsimg/sandbox.c`,
  `lib/source/source.c`, `specs/design-security.md`): the source scan reads a
  link with `fstatat(AT_SYMLINK_NOFOLLOW)` and `readlinkat()`, which the
  seccomp allowlist denies because Landlock does not mediate path-based
  metadata queries. A confined Linux worker therefore fails a tree containing
  a link with `FSIMG_EUNSUPPORTED` ("symbolic link inspection is not
  permitted", naming the link) instead of importing it. Decide how a confined
  scan reads links: allowing both calls lets a compromised worker probe
  metadata and link targets outside the source tree; an `O_PATH | O_NOFOLLOW`
  open plus `readlinkat(fd, "")` needs `O_PATH`, which the profile refuses for
  the same reason; reading links in the coordinator moves source parsing out
  of confinement. Record the decision in the security design, then test it on
  Linux with the sandbox active. FreeBSD Capsicum is expected to permit both
  calls through the source directory's rights but is unverified.

## Planned features

* [Medium] **Image minimization** (`lib/formats/fat/write-plan.c`,
  `lib/formats/exfat/write-plan.c`, `cmd/fsimg/options.c`): implement the
  [sizing design](specs/design-sizing.md): `FSIMG_WRITE_MINIMIZE` and `-M`,
  one layout pass accumulating every candidate unit's total, selection of the
  smallest volume, and the pinned test catalogs where it differs from the
  default. QNX6 follows in its writer stage.

* [Medium] **Additional inputs and formats** (`lib/source/`, `lib/formats/`):
  design manifest import, bounded partition selection, and independent NTFS and
  QNX4 reader profiles as concrete needs arise. Follow the
  [extension sequence](docs/implementation-plan.md#extension-sequence); each
  format needs a bounded-resource design and an independent compatibility corpus.

* [Medium] **QNX IFS image support** (`lib/formats/`): read, and later write,
  QNX boot images (`mkifs` output: startup header, image header, flat
  directory, packed and optionally LZO/UCL-compressed file data), such as the
  PCM 3.1 `PCM3_IFS1*.ifs` flash images. Write
  `specs/design-ifs.md` first: header variants and byte orders, compression,
  executable-in-place entries, checksums, and an independent corpus (QNX
  `dumpifs`/`mkifs` output and real images).
