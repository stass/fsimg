# QNX6 implementation and qualification

The [design](../specs/design-qnx6.md) covers independent read/write support
and variant-specific gates. The [research record](qnx6-research.md) records
sources and observations. Porsche 911 PCM 3.1 HDD compatibility drives qualification;
classic, MMI, and extended Power-Safe remain separate profiles.

* [High] **Porsche corpus and disk composition** (`lib/formats/`, volume
  selection): identify the target PCM 3.1 hardware/software and inspect authorized
  HDD samples. Record exact partition bounds, sector size, actual filesystem
  signatures, boot/locator bytes, identity, metadata, and links. Confirm whether
  live files contain unused references within their logical lengths; distinguish
  native sparse extents from a reader's missing-block zero-fill. Add synthetic
  mixed-filesystem disk fixtures. Define partition composition and a separate
  QNX4 writer if the target contains QNX4; do not label a single QNX6 volume
  as a complete compatible HDD.

* [High] **Compatibility and inode semantics** (`lib/formats/qnx6/`): obtain
  controlled native formatter output for every accepted marker/flag tuple.
  Determine clean/snapshot flags, the two version words, root modes, unused
  inode representation, status transitions, the word at inode offset 34,
  `ftime`, link encodings, mandatory `.boot` metadata, and extension trees.
  Vary one feature at a time; retain native inspection output and provenance.
  Empty and populated F1 snapshots are clues, not a writer template.

* [High] **Writable allocation recipe** (`lib/formats/qnx6/`): derive exact
  dual-system reservation, alignment, indirect-block placement, group
  parameter semantics, bitmap padding and free counts from images spanning
  direct/indirect and group boundaries. Qualify two complete fresh snapshots,
  then create/grow/delete/sync/remount under QNX. Passing a read-only parser
  cannot close this gate.

* [High] **Name and identity encodings** (`lib/formats/qnx6/`): confirm the
  long-name-checksum enable marker and disabled value; obtain non-ASCII vectors
  resolving signed-byte behavior on each target. Test 27/28/510-byte names,
  both byte orders, long-name block zero, and nonempty indirect name trees.
  Qualify UUID/text-label selection and byte order plus nonbootable boot-area
  locators without importing loader code.

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

* [Medium] **Independent infrastructure** (`test/`, `docs/build.md`): arrange
  QNX formatter/checker and isolated native write-after-create tests, a Linux
  read-only oracle, licensed redistributable fixtures or local generation,
  and a recorded target PCM compatibility run. Keep ordinary tests
  unprivileged and all third-party utilities outside runtime dependencies.
