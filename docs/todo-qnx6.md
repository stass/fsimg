# QNX6 implementation and qualification

The [format design](../specs/design-qnx6.md) covers independent read/write
support and variant-specific gates, and the
[driver design](../specs/design-qnx6-driver.md) the code that implements
them. The [research record](qnx6-research.md) records sources and
observations. Porsche 911 PCM 3.1 HDD compatibility drives qualification;
classic, MMI, and extended Power-Safe remain separate profiles.

* [High] **Porsche corpus and disk composition** (`lib/formats/`, volume
  selection): one owner HDD is
  [measured](qnx6-research.md#measured-pcm-31-hdd): five MBR/EBR partitions,
  four QNX6 and one 52.6 GB QNX4. Identify the target PCM 3.1 hardware and
  software build. Obtain an unmodified drive or a record of the original
  contents of partitions 2 and 6, which the owner reformatted. Explain why the
  head unit's `fs-qnx6.so` runs its version-3 test only when the first header
  is unreadable, and confirm on the target that one corrupt header at 8192
  sends it to the byte-0 layout ([update
  disc](qnx6-research.md#pcm-31-update-disc)). Find where the disk signature
  at MBR offsets 440 through 443 comes from. Record identity, metadata and
  links of populated volumes, and confirm whether live files contain unused
  references within their logical lengths. Define partition composition from
  the head unit's stock partition sectors (MBR plus EBR chain, 63-sector
  alignment, boot loader policy including the loader's sector fields at
  offsets 4, 8 and 12) and a separate QNX4 reader/writer design; do not label
  a single QNX6 volume as a complete compatible HDD. Add synthetic
  mixed-filesystem disk fixtures.

* [High] **Compatibility and inode semantics** (`lib/formats/qnx6/`): the
  [native QNX 6.5.0 evidence](qnx6-research.md#native-qnx-650-evidence)
  establishes the classic header words, flag classes, root and `.boot`
  inodes, status bits, link count and `ftime`. The head unit's own
  formatter matches the QNX 6.5.0 defaults by
  [disassembly](qnx6-research.md#head-unit-formatter-and-checker). Run it on
  an SH-4 QNX target with nondefault options (byte order, block size,
  checksum) to confirm, and qualify extension trees on later QNX releases.

* [High] **Writable allocation recipe** (`lib/formats/qnx6/`): qualify fsimg
  output, including the identical serial-1 header pair over a populated
  tree that no native session produces, by native
  create/grow/delete/sync/remount cycles and `chkqnx6fs`; passing a
  read-only parser cannot close this gate.

* [High] **Identity encodings** (`lib/formats/qnx6/`, writer settings): the
  UUID encoding at header offset 32 is measured; the head unit formats its
  media partition with the fixed UUID `11111111-2222-3333-4444-555555555555`.
  Expose the UUID as a writer setting and confirm on the target whether its
  software checks it.

* [High] **Writer** (`lib/formats/qnx6/`, `test/`): implement the writer
  half of the [driver design](../specs/design-qnx6-driver.md) as stage 3 of
  the [extension sequence](implementation-plan.md#extension-sequence), and
  register it only after its native and synthetic acceptance matrix. Add
  failing regressions before nontrivial fixes; run sanitizers and required
  internal/external reviews.

* [Medium] **Linux reader comparison** (`test/qnx6-integration.sh`): the
  script mounts the generated sample with the Linux qnx6 driver as root and
  compares every regular file with `fsimg cat`. No Linux result is recorded;
  run it and record the kernel revision, byte order and checksum behavior in
  the build evidence.

* [Medium] **Reader scale and fault coverage** (`test/qnx6.c`): the
  [driver design](../specs/design-qnx6-driver.md) asks for a synthetic scale
  test that grows `N`, the inode capacity, one directory's width, a file's
  depth and a file beyond 4 GiB separately, recording I/O calls, metadata
  passes and peak scratch; the native 5 GB captures cover part of this. Also
  add a bitmap tree of depth 2, an inode table spanning several top-level
  indirect blocks, a full volume with no free block, short reads and
  premature EOF, and cancellation in the middle of a traversal.

* [Medium] **Hardlink topology** (`include/fsimg.h`, `specs/design-metadata.md`):
  the [metadata design](../specs/design-metadata.md) has no hardlink
  representation, and the host scanner imports each link to a file as an
  independent copy. Add a design before any format advertises shared inodes.

* [High] **Classic-profile strictness evidence** (`lib/formats/qnx6/`): the
  driver design refuses, as unsupported, a valid block reference beyond an
  object's logical end, a bitmap longer than `ceil(N / 8)`, a nonminimal or
  depth-3 system tree, and inode types other than regular file, directory,
  symbolic link and FIFO. Confirm on QNX 6.5.0 that shrinking a file with
  `truncate` leaves no references past its new end, that the formatter never
  pads the bitmap, and whether sockets or device nodes occur on the PCM data
  volumes; also confirm that `chkqnx6fs` and the filesystem accept an
  all-zero UUID, the writer's default, and record the mode the native
  filesystem gives a symbolic link. Identify the release that commits the
  mounted flag `0x1`, as the research fixture's newer header does, and
  whether its checker accepts such a volume.

* [Medium] **Variant expansion** (`lib/formats/qnx6/`): qualify the bootless
  plain and MMI layouts in both reader and writer paths, including whether
  bootless and MMI volumes are the [version-3
  format](qnx6-research.md#head-unit-formatter-and-checker) and what MMI
  images hold at offsets 16 through 27, using the PCM head unit's filesystem
  module, which converts version 3 in both directions, as an oracle for
  version-3 writes; then 8192–32768-byte blocks, sector locators/copies,
  expandable capacity, inode integrity, sparse files, ACL/encryption metadata,
  and special inodes as evidence and test environments permit. Include
  explicit unsupported-feature fixtures from QNX 6.6/7/8. Separate sparse
  evidence from parser zero-fill fallbacks.

* [Medium] **Independent infrastructure** (`test/`, `docs/build.md`): script
  the local QNX 6.5.0 QEMU harness (unattended boot, serial shell, capture
  provenance) and give it a way to load host-built images without a reboot,
  since the guest block cache does not see host writes to the attached disk.
  Arrange isolated native write-after-create tests, a Linux
  read-only oracle, licensed redistributable fixtures or local generation,
  and a recorded target PCM compatibility run. Keep ordinary tests
  unprivileged and all third-party utilities outside runtime dependencies.
  Investigate the [PCM-Forge tool inventory](qnx6-research.md#pcm-forge-native-evidence-and-conflicting-assumptions):
  confirm `mkqnx6fs` and `chkqnx6fs` under `/mnt/data/tools/` on the matching
  target, record their identities/options and the filesystem module identity,
  and generate controlled regular-file fixtures in isolated disposable storage.
  A filesystem-only evidence collector must query these tools explicitly;
  PCM-Forge's general diagnostic tool list omits them. Acquire no unrelated
  personal persistence data and perform no formatting on production partitions.
