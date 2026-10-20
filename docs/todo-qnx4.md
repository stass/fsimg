# QNX4 qualification work

Open QNX4 work, ordered by the [QNX4 plan](qnx4-plan.md). The
[research record](qnx4-research.md) owns the evidence and provenance.

* [High] **Reader, verifier and writer** (`lib/formats/qnx4/`): stages 3 to
  7 of the plan: the reader, the verifier, interoperability, scale and fault
  coverage, and a writer registered only after QNX 6.5.0 checks, mounts,
  modifies and remounts its output. The native cycle also confirms a label
  written by fsimg, which has no native example yet.
* [Medium] **Long names, 49 to 505 bytes** (`lib/formats/qnx4/names.c`):
  stage 8. Reading and writing `.longfilenames` records; the native rename
  of a long name is still uncaptured (`mv` fails in the live shell).
* [Medium] **PCM navigation-partition profile** (`docs/qnx4-research.md`):
  stage 9. A native cycle on an fsimg volume at H1 partition 3's exact
  geometry, and the compatibility-manifest fields for the partition.
* [Low] **Head-unit acceptance** (`docs/qnx4-research.md`): the head unit's
  6.3.2 `fs-qnx4.so` and `chkfsys` have not checked fsimg output; H1 matches
  the 6.5.0 layout, but only a target run or disassembly can close this.
* [Low] **Device nodes** (`lib/formats/qnx4/`): the live CD has no `mknod`,
  so the device encoding is unobserved and devices are unsupported.
