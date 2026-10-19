# QNX6 implementation and qualification

The [design](../specs/design-qnx6.md) covers independent read/write support
and variant-specific gates. The [research record](qnx6-research.md) records
sources and observations. Porsche 911 PCM 3.1 HDD compatibility drives qualification;
classic, MMI, and extended Power-Safe remain separate profiles.

* [High] **Porsche corpus and disk composition** (`lib/formats/`, volume
  selection): one owner HDD is
  [measured](qnx6-research.md#measured-pcm-31-hdd): five MBR/EBR
  partitions, four QNX6 and one 52.6 GB QNX4. Identify the target PCM 3.1
  hardware and software build and record the head unit's mounted-device
  mapping. Obtain an unmodified drive or a record of the original contents
  of partitions 2 and 6, which the owner reformatted, and establish whether
  partition 5 is original. Record identity, metadata and links of populated
  volumes, and confirm whether live files contain unused references within
  their logical lengths. Define partition composition (MBR plus EBR chain,
  63-sector alignment, boot loader policy including the loader's sector
  fields at offsets 4, 8 and 12) and a separate QNX4 reader/writer design;
  do not label a single QNX6 volume as a complete compatible HDD. Add
  synthetic mixed-filesystem disk fixtures.

* [High] **Compatibility and inode semantics** (`lib/formats/qnx6/`): the
  [native QNX 6.5.0 evidence](qnx6-research.md#native-qnx-650-evidence)
  establishes the classic header words, flag classes, root and `.boot`
  inodes, status bits, link count and `ftime`. The PCM HDD's head-unit
  volumes match that structure but carry a different boot loader build.
  Confirm on the target's own formatter, including nondefault options (byte
  order, block size, checksum), and qualify extension trees on later QNX
  releases.

* [High] **Writable allocation recipe** (`lib/formats/qnx6/`): qualify fsimg
  output, including the identical serial-1 header pair over a populated
  tree that no native session produces, by native
  create/grow/delete/sync/remount cycles and `chkqnx6fs`; passing a
  read-only parser cannot close this gate.

* [High] **Identity encodings** (`lib/formats/qnx6/`): qualify UUID versus
  text-label selection on the PCM target.

* [High] **Reader, verifier, and writer** (`lib/formats/qnx6/`, `test/`,
  `fuzz/`): implement the bounded common tree codec, strict profile checks,
  snapshot selection, directory prepasses, content access, snapshot-aware
  ownership accounting, immutable planning, and complete initialization.
  Register each capability only after its native and synthetic acceptance
  matrix. Add failing regressions before nontrivial fixes; run sanitizers and
  required internal/external reviews for implementation stages.

* [High] **Automotive metadata fidelity** (`include/fsimg.h`, `lib/source/`,
  `lib/read/`, `lib/write/`, `cmd/fsimg/`): design explicit owner/group/mode,
  link, and additional timestamp representation needed to recreate the target
  namespace. Round-trip required fields without overloading FAT attributes
  or allowing privileged/symlink restoration through safe host extraction.

* [Medium] **Variant expansion** (`lib/formats/qnx6/`): qualify the bootless
  plain and MMI layouts in both reader and writer paths; then 8192–32768-byte
  blocks, sector locators/copies, expandable capacity, inode integrity,
  sparse files, ACL/encryption metadata, and special inodes as evidence and
  test environments permit. Include explicit unsupported-feature fixtures
  from QNX 6.6/7/8. Separate sparse evidence from parser zero-fill fallbacks.

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
