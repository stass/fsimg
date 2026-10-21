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
  `lib/formats/ifs/verify.c`, `specs/design-ifs-driver.md#verification`):
  reference matching counts only the nodes the tree emits, and inode words
  need not be unique, so a shadowed record (an earlier duplicate, or a
  whole subtree below a link) can carry the inode word of a delivered boot
  program unseen; `test/ifs.c` pins that such an image verifies. If the
  native startup finds `boot_ino` records by scanning the table rather
  than by path (untested), it could load the shadowed record's bytes while
  fsimg verifies the delivered one. Verification cannot note it without
  failing: `fsimg_verify_report_t` has no field for it, and the findings
  stream `specs/design-api.md` describes has no public interface. The
  owner chooses one of: refuse it, counting reference matches over every
  record in the table pass as well, with its status class; count it in a
  sixteenth `info` detail, `shadowed_references`, which `W` prints; or
  report it as a non-fatal finding once a findings interface exists.
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
* [Medium] **Container stages and the layer machinery** (`lib/layers/`):
  the `qnxboot` read stage can use the container reading, chunk index and
  chunk cache of `lib/layers/chunks.c`, adding LZO1X and NRV2B record kinds;
  the `hbcifs` read stage waits for the layer plan's spool (its stage 4),
  and the wrap stage for container writing (the write half of its stage 3).
  Until then compressed boot images are read through uncompressed copies as
  slices.
* [Medium] **`qnxboot` design against the implemented layers**
  (`specs/design-layers.md`, `lib/layers/`): the profile was written before
  the stack and container code. Owner decisions, before the `qnxboot` stages:
  * `qnxboot:template` is an I/O object, but layer settings are a flat
    record carried on the wire (`fsimg_layer_options_t`, 264 bytes) with no
    path or descriptor kind, and the worker's descriptor set is closed
    (`specs/design-security.md`); the template needs a coordinator-opened
    descriptor slot and its security and CLI entries.
  * The template must outlive writer construction until
    `fsimg_writer_output()` or the wrap returns; no ownership row in
    `specs/design-api.md` covers it.
  * A scheme in an `ifs` stack: the profile refuses it, while `plan.c`
    refuses only a missing partition type, as for QNX4; choose one rule.
  * The carried-volume refusal has no field in the layer record;
    `fsimg_stack_validate()` checks slot kinds only. It can be stated as the
    forced filesystem plus an empty scheme slot.
  * `qnxboot` has no rows in `layer_settings` and `layer_keys`, and the IFS
    settings rows do not mark `compress` and `template` creation-only.
  * The LZO1X transcode reads its input once per pass, while the layer
    design says wrap and compose read each input once.
  * Smaller gaps: owners of the template read, the second pass and the
    4-byte sizing; the layer report has no place to name unverified fields;
    `fsimg_image_info_t` has no place for startup-header fields (the
    `details[]` pattern fits); the spool wording names gzip only; the `.ifs`
    suffix rationale against the CLI's generic suffix list;
    `fsimg_layer_info_t` lacks carries, codec and version fields.
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
