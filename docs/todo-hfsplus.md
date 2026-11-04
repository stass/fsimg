# HFS Plus work

Open HFS Plus work, ordered by the [HFS Plus plan](hfsplus-plan.md). The
[format design](../specs/design-hfsplus.md) and the
[driver design](../specs/design-hfsplus-driver.md) describe the target; the
research record [`docs/hfsplus-research.md`](hfsplus-research.md) owns the
evidence and provenance.

* [Low] **Native evidence remainder** (`docs/hfsplus-research.md`,
  `.github/workflows/check.yml`): the `macos-15` runner's identities and attach
  check go into the record from the first CI log. A Mac OS 8.1 to 9.2 writer's
  non-ASCII names stay unobserved, and no Finder-made alias or Time Machine
  volume is in the corpus.
* [High] **Reader, compression and verifier** (`lib/formats/hfsplus/`,
  `lib/read/`, `lib/codec/`): plan stages 2 to 7. They bring four of the
  common changes the format design lists: the `FSIMG_META_EXTRA_DATA` bit,
  the 765-byte label, decoded content spans and the imported LZFSE and LZVN
  decoders.
* [Medium] **Writer and native qualification** (`lib/formats/hfsplus/`,
  `tools/hfsplus/qualify.rb`): plan stages 8 and 9, with the seconds
  helper's origin and the `hfs` partition type default. The writer stays
  unregistered until the macOS `fsck_hfs`, mount, modify and remount cycle
  is recorded.
* [Medium] **Apple data representation** (`include/fsimg.h`,
  `specs/design-metadata.md`): resource forks, extended attributes, Finder
  information and BSD flags have no place in the entry model, so the reader
  marks them with `FSIMG_META_EXTRA_DATA` and the writer stores none. A
  common design for them, through the catalog, scanner, extractor, manifest
  and CLI, would let HFS Plus round-trip them.
* [Low] **Wrapped volumes** (`lib/formats/hfsplus/volume.c`): plan stage 10,
  reading HFS Plus embedded in a classic HFS wrapper, which open refuses with
  `FSIMG_EUNSUPPORTED` until then; the `wrapper_offset` detail comes with it.
* [Low] **Directory hard links** (`lib/formats/hfsplus/catalog.c`): refused
  as an unsupported kind, which leaves Time Machine volumes unreadable past
  the first one. Following them needs a visited set and a path-explosion
  bound under its own design.
* [Low] **LZFSE chunks in DMG** (`lib/layers/dmg.c`,
  `specs/design-layers.md`): the layer design refuses `ULFO` chunks as
  unsupported. Once stage 5 imports the LZFSE decoder, the DMG reader can
  decode them through the same wrapper.
* [Medium] **bzip2 and LZMA chunks in DMG** (`lib/layers/dmg.c`,
  `specs/design-layers.md`): the layer design refuses bzip2 (`UDBZ`) and
  LZMA (`ULMO`) chunks as unsupported. Most DMGs of macOS releases from 10.4
  on are `UDBZ`, so the corpus's modern specimens (Firefox, VLC, Transmission)
  stay unreadable. An `ULMO` chunk is a plain xz stream, which the xz layer's
  liblzma binding could decode when that library is present. The first chunk
  of such a DMG is an xz stream at file offset 0, so the xz probe also claims
  the file and autodetection reports the input as ambiguous until the DMG's
  full identity (the trailer tie) outranks the bare xz magic. bzip2 needs a
  decoder fsimg does not link, a dependency decision under
  [the compression design](../specs/design-compression-libs.md).
* [Medium] **Apple Partition Map** (`lib/layers/`, `specs/design-layers.md`):
  older Mac disk images and CDs partition with APM, which the layers do not
  read, so their volumes are reachable only through `--offset` and
  `--length`. A read-only APM scheme (driver descriptor map, partition map
  entries, 512- and 2048-byte blocks) is independent of the HFS Plus reader
  and can land first. Real specimen: the Firefox 1.5.0.1 UDZO DMG
  (archive.org; `~/fsimg-corpus/edge/dmg/`) holds a DDM, an APM and one
  `Apple_HFS` partition carrying HFS Plus.
