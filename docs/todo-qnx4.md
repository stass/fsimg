# QNX4 qualification work

Open QNX4 work, ordered by the [QNX4 plan](qnx4-plan.md). The
[research record](qnx4-research.md) owns the evidence and provenance.

* [High] **Writer registration after native qualification**
  (`lib/formats/qnx4/descriptor.c`, `tools/qnx/guest/qualify4.sh`): stage 7.
  The writer is implemented and tested but unregistered: it lives in
  `fsimg_qnx4_candidate_format`, outside the registry, so `formats` and
  `create -t qnx4` do not offer it. Run `qualify4.sh` in the QNX 6.5.0 guest
  on the `test-qnx4-write --create` images ([build](build.md) lists them):
  the 64 MiB tree with a label, the smallest volume, 1, 2 and 3 bitmap
  blocks and a nearly full volume. Record the cycle in the research record,
  then register: move the writer and its write masks into
  `fsimg_qnx4_format`, delete the candidate, switch `test/qnx4-write.c` to
  `fsimg_writer_new()`, and update `README.md`, `docs/fsimg.1`,
  `docs/libfsimg.3` and `NEWS`. The cycle also confirms a label written by
  fsimg natively, beyond the byte comparison with the `-l` captures.
* [High] **Interoperability and scale** (`lib/formats/qnx4/`): stage 5 of
  the plan, the reader's interoperability, scale and fault coverage, and the
  writer's scale cases (`N` near `2^31` on a sparse sink, a spilling
  directory, and the 2 GiB object limit at 524288 directory steps and
  4194289 long names, which `object_fits()` refuses and no test reaches).
* [Medium] **Long names, 49 to 505 bytes, natively** (`lib/formats/qnx4/`):
  stage 8. The reader decodes `.longfilenames` records and checks them
  against the `q4-long` capture, and the writer emits them with the mangled
  prefix and back-references; native QNX has not yet checked, read, renamed
  or deleted a long name fsimg wrote, and the native rename of a long name
  is uncaptured (`mv` fails in the live shell).
* [Medium] **48-byte labels through the CLI** (`cmd/fsimg/job.c`,
  `cmd/fsimg/worker.h`): the job record carries a label of at most 47 bytes,
  so `create -L` with a 48-byte QNX4 label would be refused as an invalid
  job. The library's writer holds 48 bytes; widen the job record before the
  writer is registered.
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
* [Low] **"Owned block is marked free" untested**
  (`lib/formats/qnx4/verify.c`): the branch is reachable only when
  `2 + B` exceeds the 32768 blocks open's first bitmap window checks, so
  `N` above about 134 million blocks; the stage 5 sparse scale fixture can
  reach it.
* [Low] **Integration run time** (`test/qnx4-integration.sh.in`): the
  native comparison starts one `fsimg cat` per regular file, about four
  minutes for the 40 captures on macOS; a single command that checksums
  every file would make it seconds.
* [Medium] **PCM navigation-partition profile** (`docs/qnx4-research.md`):
  stage 9. A native cycle on an fsimg volume at H1 partition 3's exact
  geometry, and the compatibility-manifest fields for the partition.
* [Low] **Head-unit acceptance** (`docs/qnx4-research.md`): the head unit's
  6.3.2 `fs-qnx4.so` and `chkfsys` have not checked fsimg output; H1 matches
  the 6.5.0 layout, but only a target run or disassembly can close this.
* [Low] **Device nodes** (`lib/formats/qnx4/`): the live CD has no `mknod`,
  so the device encoding is unobserved and devices are unsupported.
* [Low] **Synthetic `.inodes` growth** (`test/qnx4.c`): no generated volume
  holds more than 128 table records, so `.inodes` growing through its own
  extent block is exercised only by the `q4-inodes` capture and fuzzing.
