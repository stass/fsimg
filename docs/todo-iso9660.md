# ISO 9660 work

Open ISO 9660 work, ordered by the [ISO 9660 plan](iso9660-plan.md). The
[format](../specs/design-iso9660.md) and
[driver](../specs/design-iso9660-driver.md) designs are the contract.

## Scheduled

* [High] **Oracles and corpus** (`test/iso9660-corpus.sh`, `docs/build.md`):
  stage 0. Remaining evidence: Linux/FreeBSD kernel mounts; executable
  OVMF/SeaBIOS boots; genisoimage and makefs producer/consumer measurements;
  pycdlib integration and measurements; writer-image questions (36-byte PX,
  extent-zero empty files, Rock Ridge on only the first multi-extent section,
  oversized EFI sector counts, relocation and `.rr_moved` visibility, Joliet
  `;1` identifiers and kernel/Windows consumers, ISO selection in hybrids,
  and version-order ties); the 65,535-directory fixture with producer,
  consumer and writer measurements; and bootable corpus images with an EFI
  image in an appended partition past the volume (`xorriso
  -append_partition` with `--interval:appended_partition_2`, as Ubuntu
  images are built) and with initial-entry selection criteria
  (`-eltorito-selcrit`), which the El Torito comparison does not yet
  cover.
* [High] **Reader** (`lib/formats/iso9660/`, `test/iso9660.c`,
  `fuzz/iso9660.c`): stages 5, 7 and 8, the Joliet view, the verifier,
  and interoperability, scale and fault evidence. The registered reader
  presents the Rock Ridge and primary views:
  * `view=auto` never selects `joliet`, and a forced `joliet` fails open
    as unsupported, until stage 5 completes the
    [selection rule](../specs/design-iso9660.md#view-selection);
  * the format record's profile (`rockridge/primary`) gains `joliet` in
    stage 5, and the man page's ISO paragraph and the README row change
    with it;
  * a Rock Ridge directory carrying `RE` that no `CL` names is left out of
    the view without an error, with its subtree, until stage 7's
    relocation match refuses it under `W`;
  * the format has no verifier, so `W` refuses it as unsupported, until
    stage 7.
* [Low] **Root metadata** (`lib/read/`, `lib/formats/iso9660/`): the
  format design gives the Rock Ridge root the owner, mode and times of its
  own first dot record, but the walker delivers no metadata for the root
  in any format, so the reader decodes and checks that stream and drops
  them. A common root-metadata path would serve extraction of the root's
  mode as well.
* [Low] **Naming the refused entry** (`lib/formats/iso9660/directory.c`,
  `lib/read/directory.c`): the driver design asks that an associated,
  interleaved or other refused record fail preparation naming the entry.
  A preparation error carries no path for any driver, and the driver
  cannot name the directory's path, so the message names the condition
  only. A common way for `prepare` to name a child would serve every
  driver.
* [High] **Writer** (`lib/formats/iso9660/write-*.c`,
  `test/iso9660-write.c`): stages 9 and 10. The writer lands unregistered
  and is registered after every stage 10 consumer agrees in every view.
  Registration documents `level=`, `boot=`, `efiboot=` and `bootload=` in
  the creation `-o` help and the man page's CREATION settings.
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
