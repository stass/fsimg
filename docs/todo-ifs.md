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
  the native uncompressed layout the `ifsboot` profile assumes for
  compression `0x00` (`stored_size = startup_size + imagefs_size`, no stored
  trailer); the inode-flag nibble the writer's boot settings give referenced
  records, whose closed table is empty until then, so every `boot1=` to
  `boot4=` and `script=` setting is refused and the wrap's native boot gates
  use a native bootable body; and how `procnto` resolves a duplicated path
  (owner decision 10). They answer owner decision 6 and decision 10's growth
  refusal, and gate the wrap stage.
  * [Low] Boot a body whose shadowed record carries `boot_ino[0]`'s word,
    to learn whether the startup finds boot programs by path or by
    scanning the table, which `shadowed_references` only counts.
* [Medium] **`dumpifs -u` copies of the H3 boot images**
  (`docs/ifs-research.md`): the eight images exceed `load.py`'s 3 MiB
  budget. Write them into `big.img` while the VM is stopped, run
  `dumpifs -u` into `/dev/shmem` and compare with the oracle decodes, before
  the decoder stage's gate.
* [Low] **External ImageFS readers** (`test/ifs-integration.sh`): build
  `lclevy/dumpifs` and `qnxprobe` at pinned revisions outside the tree and
  run the integration script with `DUMPIFS` and `QNXPROBE`, then with
  `FSIMG_IFS_IMAGES` once uncompressed S1 and S2 copies exist; record the
  results in `docs/build.md`, where both are skips. Whether either exits
  nonzero on an image it rejects is unconfirmed; the script counts a zero
  exit as acceptance.
* [Low] **ImageFS scale RSS bound off macOS** (`test/ifs.c`,
  `docs/build.md`): `make check-scale` asserts the 32 MiB RSS growth bound
  on every host, but it has been measured only on macOS, under
  `MallocSpaceEfficient=1`. Run it on FreeBSD and Linux, whose allocators
  also keep freed pages resident, and record the figures or set the
  allocator's return policy there too.
* [Low] **Decoder manifest framing** (`tools/qnx/decode/ifsdump.py`,
  `test/ifs-native.py`): `--manifest` writes names and link targets raw in
  tab- and newline-separated lines, so a capture whose name or target holds
  either byte fails the comparison as a decoder-manifest mismatch although
  fsimg may be right. Escape both fields (hex, as `test-ifs --manifest`
  writes targets) when a capture needs it. The CLI's refused count is not
  cross-checked against `test-ifs --native`'s refusal classes either.
* [Low] **Native `dumpifs` cycle in the integration script**
  (`test/ifs-integration.sh`, `tools/qnx/`): the driver design's
  integration paragraph runs the QNX 6.5.0 guest's `dumpifs` list, check
  and extract cycle over the reader's fixtures; the script has no step that
  starts the guest. Add it, skipped when the VM is absent, with the writer's
  native qualification harness, which drives the same guest.
* [Medium] **`hbcifs` header fields** (`specs/design-layers.md`): the
  fields at `0x10`, `0x14`, `0x18` and `0x1c` of `PCM3_IFS2.ifs` are
  unidentified; `0x14` is neither CRC-32 nor the additive sum. Until one is
  identified as the checksum, `hbcifs` verification is incomplete (owner
  decision 8).
* [Low] **Carried-volume rule placement** (`lib/layers/stack.c`,
  `plan.c`, `image.c`): `fsimg_stack_carried` runs in stack validation,
  in the disk plan, because the writer's and output's `fsimg_stack_parse`
  callers do not validate, and at a detected claim, which then rewrites the
  image's forced slots. One parse-and-validate funnel would delete the plan
  call, and deriving the carried filesystem from the opened container would
  keep the forced stack what the caller forced.
* [Low] **Worker image records against the registry**
  (`cmd/fsimg/protocol.c`, `worker-parent.c`): the coordinator could
  require an IMAGE record's unverified list to equal the container's
  profile list, details only from a container that reports them, and a
  spool-backed container's disk within the spool limit.
* [Low] **Forced spool-backed container under forced gzip**
  (`lib/layers/image.c`): with both forced, the whole gzip member is
  decoded into the spool before the claim refuses a second spool;
  `validate_forced_stack` could refuse the pair before any byte is read.
* [Low] **Detail type naming** (`include/fsimg.h`): `fsimg_volume_detail_t`
  also carries container details; a neutral type name would say so before
  the API is released.
* [Medium] **Container stages and the layer machinery** (`lib/layers/`):
  the `ifsboot` write stage waits for container writing (the write half of
  the layer plan's stage 3); until then `ifsboot` is registered read only,
  with no plan, bound or encoder, and writing it is refused naming it.
* [Medium] **`ifsboot` design against the implemented layers**
  (`specs/design-layers.md`, `lib/layers/`): the profile was written before
  the stack and container code. Owner decisions, before the `ifsboot` stages:
  * `ifsboot:template` is an I/O object, but layer settings are a flat
    record carried on the wire (`fsimg_layer_options_t`, 264 bytes) with no
    path or descriptor kind, and the worker's descriptor set is closed
    (`specs/design-security.md`); the template needs a coordinator-opened
    descriptor slot and its security and CLI entries.
  * The template must outlive writer construction until
    `fsimg_writer_output()` or the wrap returns; no ownership row in
    `specs/design-api.md` covers it.
  * `ifsboot` has no rows in `layer_settings` and `layer_keys`, and the IFS
    settings rows do not mark `compress` and `template` creation-only.
  * The LZO1X transcode reads its input once per pass, while the layer
    design says wrap and compose read each input once.
  * Smaller gaps: owners of the template read, the second pass and the
    4-byte sizing; the `.ifs` suffix rationale against the CLI's generic
    suffix list; `fsimg_layer_info_t` lacks carries and codec version
    fields.
* [Medium] **Exact volumes in unnamed wraps and composes**
  (`lib/layers/output.c`, `specs/design-layers.md#exact-volumes`): the
  disk plan applies the exact-volume rule to the filesystem a stack names,
  so `fsimg_image_wrap()` of an ImageFS volume under `vhd` or `gpt`, with
  no filesystem in the stack, and `fsimg_image_compose()` with an ImageFS
  part still write a padded ImageFS that `W` reports as longer than its
  image size. Both are library-only. The owner chooses whether the rule
  follows the volume's identified filesystem: wrap already identifies its
  input and could name it in the stack before planning; compose identifies
  only untyped parts.
* [Low] **Large native bodies** (`tools/qnx/guest/popifs.sh`): the guest's
  4 MiB RAM disk limits native builds to small bodies; build one above
  16 MiB on a scratch volume if a design rule ever depends on body size.
* [Medium] **Writer scale** (`test/ifs-write.c`): the 2^32 - 4 byte limit
  is emitted on a sparse sink; a catalog that spills to scratch, with
  measured I/O calls, scratch and RSS as the
  [driver design](../specs/design-ifs-driver.md#tests) asks, is not.
