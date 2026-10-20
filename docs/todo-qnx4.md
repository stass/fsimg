# QNX4 qualification work

Open QNX4 work, ordered by the [QNX4 plan](qnx4-plan.md). The
[research record](qnx4-research.md) owns the evidence and provenance.

* [High] **Interoperability, scale and writer** (`lib/formats/qnx4/`):
  stages 5 to 7 of the plan: interoperability, scale and fault coverage,
  and a writer registered only after QNX 6.5.0 checks, mounts, modifies and
  remounts its output. The native cycle also confirms a label written by
  fsimg, which has no native example yet.
* [Medium] **Writing long names, 49 to 505 bytes**
  (`lib/formats/qnx4/names.c`): stage 8. The reader decodes
  `.longfilenames` records and checks them against the `q4-long` capture;
  writing them remains, and the native rename of a long name is still
  uncaptured (`mv` fails in the live shell).
* [Medium] **48-byte labels through the CLI** (`cmd/fsimg/job.c`,
  `cmd/fsimg/worker.h`, `lib/write/writer.h`): the job record and the
  writer options carry a label of at most 47 bytes, so `create -L` with a
  48-byte QNX4 label would be refused as an invalid job. Widen both with
  the writer stage.
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
