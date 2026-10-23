# QNX IFS qualification work

Open QNX IFS work, ordered by the [IFS plan](ifs-plan.md). The
[research record](ifs-research.md) owns the evidence and provenance.

* [Low] **Boot an x86 image without the BIOS prefix**
  (`tools/qnx/vm/bootifs.sh`, `docs/ifs-research.md`), the one boot of
  E8's plan row not run, to learn whether the `ifsboot` writer's machine 3
  entry can write a prefixless image that boots.
  * [Low] Boot a body whose shadowed record carries `boot_ino[0]`'s word,
    to learn whether the startup finds boot programs by path or by
    scanning the table, which `shadowed_references` only counts.
* [Medium] **An NRV2B encoder for compressed x86 boot images**
  (`lib/codec/`, `lib/layers/ifsboot.c`; owner decision 3): E14 shows that
  6.5.0's `startup-bios` decodes UCL NRV2B bodies and rejects LZO ones,
  `mkifs`'s own included, so machine 3 is writable under `none` only and
  an x86 image the writer packages is as large as its ImageFS. A
  compressed x86 image needs an NRV2B encoder: one more codec in the codec
  design under the plan's clean-room split, a write entry for compression
  `0x0c` in the profile's compression table with the framing unchanged,
  and its own boot under the x86 gate before the machine table lists it.
  Owner decision 3 puts only LZO1X encoding in scope, so the owner decides
  whether to schedule it.
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
* [Medium] **Required `ifsboot` keys in four tables** (`lib/layers/ifsboot.c`,
  `lib/layers/stack.c`, `cmd/fsimg/options.c`, `cmd/fsimg/job.c`): the seven
  keys without a default are named in `ifsboot_plan`'s required list, and
  their presence flags in the library's settings table, the option parser
  and the job record. The job record's layout assertion catches a new
  field, but nothing ties a key added to the settings table to the
  required list. A `required` column in the library's settings table,
  checked by the layer-options check of a write, would make the plan's
  list unnecessary.
* [Low] **File setting aliasing standard output** (`cmd/fsimg/paths.c`):
  the library's alias check needs the sink's identity, and the CLI's sink
  is private staging or the data channel, so `fsimg c ... -o
  ifsboot:startup=x - > x` is not refused as an alias: the shell has
  truncated `x`, and the image has no code or fails as changed, by relay
  timing. Create roots have the same gap. Comparing `fstat(STDOUT_FILENO)`
  with each setting's and root's identity in the coordinator would close
  it.
* [Low] **Wrap verifies before cheap refusals** (`lib/layers/output.c`):
  `fsimg_image_wrap()` verifies its input whole before the disk plan
  checks the layer settings, so a missing `ifsboot:machine` or a read-only
  carried container is refused only after the input is read and summed.
  The refusal still precedes any output; running the plan's argument checks
  first would save the work.
* [Low] **`.ifs` suffix** (`specs/design-layers.md`, `cmd/fsimg/`): the
  rationale for `ifsboot` having no file suffix, against the CLI's generic
  suffix list, is unstated.
* [Low] **Command-line wrap and unwrap** (`cmd/fsimg/`,
  `specs/design-cli.md`): wrap, unwrap and compose are library operations
  without a command-line form, so rebuilding a native boot image around
  a new ImageFS from the shell needs a program against the library. A
  command-line form comes for every container together.
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
