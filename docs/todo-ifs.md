# QNX IFS qualification work

Open QNX IFS work, ordered by the [IFS plan](ifs-plan.md). The
[research record](ifs-research.md) owns the evidence and provenance.

* [Medium] **Boot experiments E8 and E12** (`tools/qnx/guest/popifs.sh`,
  `docs/ifs-research.md`): x86 `startup-bios` and `procnto` images,
  uncompressed and with each codec, with and without the BIOS prefix, the
  growth and offset-dependence variants, raw members, and a QEMU boot
  method with boot detection. They answer owner decision 6 and decision
  10's growth refusal, and gate the wrap stage. Specifically: the native
  uncompressed layout the `qnxboot` profile assumes for compression `0x00`
  (`stored_size = startup_size + imagefs_size`, no stored trailer); whether
  `mkifs` changes any startup field beyond the derived ones when an image
  grows (the wrap's growth refusal stays until it is known); whether
  processed members' bytes depend on their image offset (if so, the wrap
  scope returns to the owner); and the inode-flag nibble the writer's boot
  settings give referenced records, whose closed table is empty until then:
  every `boot1=` to `boot4=` and `script=` setting is refused, so the
  wrap's native boot gates use a native bootable body until it is filled. The live CD's El Torito
  entry is a 2.88 MB floppy image with its own loader, so a rebuilt ISO
  with a replaced `boot/fs/qnxbase.ifs` is the first boot candidate. The
  same boot test settles how `procnto` resolves a duplicated path (owner
  decision 10).
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
* [Medium] **Public API additions** (`include/fsimg.h`, worker job record):
  the volume information's `mountpoint` field and valid bit, the detail
  table, `FSIMG_SETTING_MOUNTPOINT`, `FSIMG_SETTING_BOOT` and the emission
  copy observer are specified in `specs/design-api.md` and
  `specs/design-emission.md`; they land with the reader and writer stages.
