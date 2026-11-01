# HFS Plus work

Open HFS Plus work, ordered by the [HFS Plus plan](hfsplus-plan.md). The
[format design](../specs/design-hfsplus.md) and the
[driver design](../specs/design-hfsplus-driver.md) describe the target; the
research record `docs/hfsplus-research.md`, which stage 1 creates, will own
the evidence and provenance.

* [High] **Native evidence corpus** (`tools/hfsplus/`,
  `docs/hfsplus-research.md`): plan stages 0 and 1 have not run. The reader's
  closed tables, the compression layouts, the writer's native defaults and,
  above all, the E3 Unicode observation that the generated fold and
  decomposition tables must match row for row all wait on it.
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
* [Medium] **Volume identifier setting** (`specs/design-hfsplus.md`): owner
  decision 5 of the plan, open. The design takes the 64-bit identifier from
  the first eight bytes of `volume_uuid`; settle it, or replace it with a
  dedicated setting or zero, before stage 8.
* [Medium] **Apple data representation** (`include/fsimg.h`,
  `specs/design-metadata.md`): resource forks, extended attributes, Finder
  information and BSD flags have no place in the entry model, so the reader
  marks them with `FSIMG_META_EXTRA_DATA` and the writer stores none. A
  common design for them, through the catalog, scanner, extractor, manifest
  and CLI, would let HFS Plus round-trip them.
* [Low] **Wrapped volumes** (`lib/formats/hfsplus/volume.c`): plan stage 10,
  reading HFS Plus embedded in a classic HFS wrapper.
* [Low] **Directory hard links** (`lib/formats/hfsplus/catalog.c`): refused
  as an unsupported kind, which leaves Time Machine volumes unreadable past
  the first one. Following them needs a visited set and a path-explosion
  bound under its own design.
* [Low] **LZFSE chunks in DMG** (`lib/layers/dmg.c`,
  `specs/design-layers.md`): the layer design refuses `ULFO` chunks as
  unsupported. Once stage 5 imports the LZFSE decoder, the DMG reader can
  decode them through the same wrapper.
* [Low] **Apple Partition Map** (`lib/layers/`, `specs/design-layers.md`):
  older Mac disk images partition with APM, which the layers do not read,
  so their HFS Plus volumes are reachable only through `--offset` and
  `--length`.
