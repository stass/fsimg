# QNX4 qualification work

Open QNX4 work, ordered by the [QNX4 plan](qnx4-plan.md). The
[research record](qnx4-research.md) owns the evidence and provenance.

* [Medium] **Linux and qnxprobe comparisons not yet run**
  (`test/qnx4-integration.sh`): the script compares the Linux qnx4 driver's
  view of the generated sample, every native capture and writer output with
  fsimg's, and qnxprobe's acceptance of the same volumes, but it has run
  only on macOS, where both are skips. Run it with `FSIMG_QNX4_LINUX=1` as
  root on a Linux host and record the kernel revision in [build](build.md);
  pin a qnxprobe revision, confirm that `$QNXPROBE IMAGE` is its invocation,
  and record the run.
* [Low] **Claims as a checked fact** (`lib/formats/qnx4/extent.c`): a walk
  under the allocated policy trusts that an earlier exclusive walk claimed
  the storage (the root at open, a later name of an `.inodes` object).
  Testing that every block is already claimed would make that a checked
  fact, but needs the root claimed at open instead of by `prepare`, which
  changes the driver design's open sequence.
* [Low] **Verifier diagnostics carry no location** (`lib/formats/qnx4/verify.c`,
  `extent.c`): "allocated block has no owner" and "block is already in use"
  name neither the block nor its owners. `fsimg_error_t`'s offset could
  carry the block's volume offset without quoting image bytes.
* [Low] **Integration run time** (`test/qnx4-integration.sh.in`): the
  native comparison starts one `fsimg x -O` per regular file, about four
  minutes for the 41 captures on macOS; a single command that checksums
  every file would make it seconds.
* [Low] **Head-unit acceptance** (`docs/qnx4-research.md`): the head unit's
  6.3.2 `fs-qnx4.so` and `chkfsys` have not checked fsimg output; H1 matches
  the 6.5.0 layout, but only a target run or disassembly can close this.
* [Low] **Device nodes** (`lib/formats/qnx4/`): the live CD has no `mknod`,
  so the device encoding is unobserved and devices are unsupported.
* [Low] **Partition types for layers** (`specs/design-layers.md`): the
  layer design's GPT and MBR type tables have no `qnx4` row, so a scheme
  around a QNX4 volume has no derived type. Record the MBR types QNX 6.5.0
  assigns to QNX4 partitions from native evidence, and whether any GPT type
  exists, before the stacks stage.
