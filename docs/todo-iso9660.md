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
  and version-order ties); and the 65,535-directory fixture with producer,
  consumer and writer measurements.
* [High] **Reader** (`lib/formats/iso9660/`, `test/iso9660.c`,
  `fuzz/iso9660.c`, `fuzz/iso9660-susp.c`): stages 4 to 8, the Rock Ridge
  and Joliet views, El Torito inspection, the verifier, and
  interoperability, scale and fault evidence. The registered reader
  presents the primary view only:
  * `view=auto` selects `primary` without the handshake, and forced
    `rockridge` and `joliet` fail open as unsupported, until stages 4 and
    5 implement the [selection rule](../specs/design-iso9660.md#view-selection);
  * the format record's profile (`primary`), entry kinds, metadata fields
    and name bound grow with those views, and the man page's ISO paragraph
    and the README row change with them;
  * the `rockridge` detail is 3 (unknown) until the handshake runs at
    open, and the `boot_catalog_block`, `boot_entries`, `boot_x86` and
    `boot_efi` details are added with El Torito in stage 6;
  * the format has no verifier, so `W` refuses it as unsupported, until
    stage 7.
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
