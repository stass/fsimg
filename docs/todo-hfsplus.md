# HFS Plus work

Open HFS Plus work. The [format design](../specs/design-hfsplus.md) and the
[driver design](../specs/design-hfsplus-driver.md) describe the target; the
research record [`docs/hfsplus-research.md`](hfsplus-research.md) owns the
evidence and provenance.

* [Low] **Native evidence remainder** (`docs/hfsplus-research.md`,
  `.github/workflows/check.yml`): the `macos-15` runner's identities and attach
  check go into the record from the first CI log. A Mac OS 8.1 to 9.2 writer's
  non-ASCII names stay unobserved, and no Finder-made alias or Time Machine
  volume is in the corpus.
* [Medium] **Default output budget on a compressed volume** (`lib/read/reader.c`):
  the default output budget is the slice, and a compressed file charges its
  decoded size, so a volume whose files decode to more than its own length
  fails with `FSIMG_ELIMIT` unless the caller raises the limit. No capture or
  corpus specimen reaches it. The owner decides whether a compressed volume
  funds a larger default, since the budget is also what bounds a bomb across
  many files.
* [Low] **Large-catalog qualification verdict** (`tools/hfsplus/qualify.rb`):
  `qualify.rb --large-catalog` has not finished with results on the writer that
  states the low 32 bits of a fork past `2^32` bytes as its `clumpSize`. `fsck_hfs
  -fn`, a mount and `fsimg W` accept that volume, but the compare and modify steps
  have no recorded verdict. A run takes about 2.5 hours and 8 GiB of disk.
* [Medium] **Apple data representation** (`include/fsimg.h`,
  `specs/design-metadata.md`): resource forks, extended attributes, Finder
  information and BSD flags have no place in the entry model, so the reader
  marks them with `FSIMG_META_EXTRA_DATA` and the writer stores none. A
  common design for them, through the catalog, scanner, extractor, manifest
  and CLI, would let HFS Plus round-trip them.
* [Low] **Directory hard links** (`lib/formats/hfsplus/catalog.c`): refused
  as an unsupported kind, which leaves Time Machine volumes unreadable past
  the first one. Following them needs a visited set and a path-explosion
  bound under its own design.
* [Low] **LZFSE chunks in DMG** (`lib/layers/dmg.c`,
  `specs/design-layers.md`): the layer design refuses `ULFO` chunks as
  unsupported. The DMG reader can decode them with the LZFSE decoder of
  `lib/codec/` through the same wrapper.
* [Medium] **LZMA chunks in DMG** (`lib/layers/dmg.c`,
  `specs/design-layers.md`): the layer design refuses LZMA (`ULMO`) chunks as
  unsupported. An `ULMO` chunk is a plain xz stream, which the xz layer's
  liblzma binding could decode when that library is present. The first chunk
  of such a DMG is an xz stream at file offset 0, so the xz probe also claims
  the file and autodetection reports the input as ambiguous until the DMG's
  full identity (the trailer tie) outranks the bare xz magic.
* [Low] **`W` on xorriso HFS Plus volumes** (`lib/`, `specs/design-hfsplus.md`):
  the `Apple_HFS` partition of `supergrub2-classic-*` (`-F apm`) lists and reads,
  and `W` fails with "bitmap marks a block nothing owns". Decide from a closer look
  whether xorriso reserves blocks a verifier should accept as it does the
  formatter-marked tail, or whether the verdict stands and belongs in the record.
* [Low] **Linux kernel mount comparison** (`test/hfsplus-integration.rb`):
  compare the fsimg baseline against a read-only Linux kernel mount in a root
  Linux VM. The comparison and its first run belong together, including the
  driver's name mapping as observed there, since unrun comparison code is
  worth nothing.
