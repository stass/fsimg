# ISO 9660 work

Open ISO 9660 work, ordered by the [ISO 9660 plan](iso9660-plan.md). The
[format](../specs/design-iso9660.md) and
[driver](../specs/design-iso9660-driver.md) designs are the contract.

## Scheduled

* [High] **Owner acceptance of the designs** (`specs/design-iso9660.md`,
  `specs/design-iso9660-driver.md`, `docs/iso9660-plan.md`): the stage 1
  gate. The owner accepts the designs and confirms or changes
  [owner decisions](iso9660-plan.md#owner-decisions) 5 to 14, which carry a
  recommendation but are not yet settled.
* [High] **Oracles and corpus** (`test/iso9660-corpus.sh`, `docs/build.md`):
  stage 0. Install the producers and consumers of the plan's input table,
  record their versions, generate the corpus, survey the H3 disc for counts,
  and answer the [evidence questions](iso9660-plan.md#evidence-questions).
  None of the oracles is installed in the cloud development container.
* [High] **Common mechanisms** (`lib/read/`, `lib/write/`,
  `lib/core/settings.c`, `cmd/fsimg/job.c`, `include/fsimg.h`): stage 2.
  Decoded link targets, the byte-name profile rename
  (`FSIMG_NAME_PROFILE_QNX` to `FSIMG_NAME_PROFILE_BYTES`), several walks
  in one verification, the link-target read, the `view` read option with the
  `read_settings` capability mask, and the ISO write options, each with
  tests independent of the driver.
* [High] **Reader** (`lib/formats/iso9660/`, `test/iso9660.c`,
  `fuzz/iso9660.c`, `fuzz/iso9660-susp.c`): stages 3 to 8, the primary,
  Rock Ridge and Joliet views, El Torito inspection, the verifier, and
  interoperability, scale and fault evidence.
* [High] **Writer** (`lib/formats/iso9660/write-*.c`,
  `test/iso9660-write.c`): stages 9 and 10. The writer lands unregistered
  and is registered after every stage 10 consumer agrees in every view.
* [Medium] **Deep trees by relocation** (`lib/formats/iso9660/write-plan.c`):
  stage 11. Until it lands, planning refuses a tree that needs relocation,
  by depth or by path length, naming the first directory to relocate.
* [Medium] **El Torito writer** (`lib/formats/iso9660/eltorito.c`,
  `write-emit.c`): stage 12, with the SeaBIOS and OVMF boot gates. The EFI
  gate needs a UEFI application whose license allows it in the tree, or one
  supplied from the environment.
* [Medium] **Specification-driven rules** (`test/spec/lib/fsimg_spec/iso9660/`):
  stage 13, the harness plug-in citing the format design's rules.

## Unscheduled

Each needs its own evidence or design before it is scheduled.

* [Low] **Boot information table** (`lib/formats/iso9660/`): `isolinux` and
  GRUB's BIOS El Torito images need the 56-byte table patched into the
  image at offset 8. Writing it makes a member's bytes differ from its
  source, which the content and emission contracts do not describe; it
  needs a design if BIOS boot with those loaders is wanted.
* [Low] **Hidden boot images and emulation boot**: boot images outside the
  tree, as `xorriso` writes them, and floppy or hard-disk emulation entries.
* [Low] **Hybrid system areas**: writing an isohybrid MBR or GPT around the
  ISO, which overlaps the layer design's scheme slot and needs a
  composition design.
* [Low] **Enhanced descriptors and interleaved files**: the ISO 9660:1999
  view and interleaved content, if a target image needs them.
* [Low] **zisofs**: transparent decoding of `ZF` files through the system
  zlib, which the dependency policy permits; it needs a content design for
  compressed extents.
* [Low] **Raw optical sectors**: a container layer for 2352-byte sectors
  and BIN/CUE, beneath the same driver.
