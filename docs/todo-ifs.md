# QNX IFS qualification work

Open QNX IFS work, ordered by the [IFS plan](ifs-plan.md). The
[research record](ifs-research.md) owns the evidence and provenance.

* [Medium] **E8 and the remaining E12 boots** (`tools/qnx/guest/popifs.sh`,
  `docs/ifs-research.md`), each run with the [E12 boot
  method](ifs-research.md#paths-below-a-link-at-boot-e12): LZO bodies and an
  image without the BIOS prefix; whether `mkifs` changes any startup field
  beyond the derived ones when an image grows (the wrap's growth refusal
  stays until it is known); whether processed members' bytes depend on their
  image offset (if so, the wrap scope returns to the owner); raw members;
  the native uncompressed layout the `qnxboot` profile assumes for
  compression `0x00` (`stored_size = startup_size + imagefs_size`, no stored
  trailer); the inode-flag nibble the writer's boot settings give referenced
  records, whose closed table is empty until then, so every `boot1=` to
  `boot4=` and `script=` setting is refused and the wrap's native boot gates
  use a native bootable body; and how `procnto` resolves a duplicated path
  (owner decision 10). They answer owner decision 6 and decision 10's growth
  refusal, and gate the wrap stage.
* [Medium] **References and shadowed records** (`lib/formats/ifs/tree.c`,
  `lib/formats/ifs/volume.c`, the verifier,
  `specs/design-ifs-driver.md#verification`): reference matching counts
  only the nodes the tree emits, and inode words need not be unique, so a
  shadowed record (an earlier duplicate, or a whole subtree below a link)
  can carry the inode word of a delivered boot program unseen. If the
  native startup finds `boot_ino` records by scanning the table rather
  than by path (untested), it could load the shadowed record's bytes while
  fsimg verifies the delivered one. Recommended: count reference matches
  over every record in the table pass as well, and have verification
  refuse a reference that a shadowed record also carries; test it when
  verification lands, with the acceptance matrix's "naming only a
  shadowed record" cases. The reference rule is an owner decision.
* [Medium] **`dumpifs -u` copies of the H3 boot images**
  (`docs/ifs-research.md`): the eight images exceed `load.py`'s 3 MiB
  budget. Write them into `big.img` while the VM is stopped, run
  `dumpifs -u` into `/dev/shmem` and compare with the oracle decodes, before
  the decoder stage's gate.
* [Medium] **`hbcifs` header fields** (`specs/design-layers.md`): the
  fields at `0x10`, `0x14`, `0x18` and `0x1c` of `PCM3_IFS2.ifs` are
  unidentified; `0x14` is neither CRC-32 nor the additive sum. Until one is
  identified as the checksum, `hbcifs` verification is incomplete (owner
  decision 8).
* [Medium] **Container stages wait for the layers** (`lib/layers/`): the
  `qnxboot` read, `hbcifs` read and wrap stages of the plan need layer
  stages 3 and 4 (containers, the chunk index and the spool). Until then
  compressed boot images are read through uncompressed copies as slices.
* [Low] **Large native bodies** (`tools/qnx/guest/popifs.sh`): the guest's
  4 MiB RAM disk limits native builds to small bodies; build one above
  16 MiB on a scratch volume if a design rule ever depends on body size.
* [Low] **Directory times in writer output** (`specs/design-ifs-driver.md`):
  `mkifs` writes time 0 on every directory record, and the writer stores the
  catalog's time. The native qualification cycle checks that `dumpifs -x`
  accepts nonzero directory times; the result returns to the owner.
* [Medium] **Writer API additions** (`include/fsimg.h`, worker job record):
  `FSIMG_SETTING_MOUNTPOINT`, `FSIMG_SETTING_BOOT` and the emission copy
  observer are specified in `specs/design-api.md` and
  `specs/design-emission.md`; they land with the writer stage.
