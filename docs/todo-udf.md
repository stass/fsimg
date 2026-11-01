# UDF qualification work

Open UDF work, ordered by the [UDF plan](udf-plan.md). The
[research record](udf-research.md) owns the evidence and provenance; the
[format](../specs/design-udf.md) and [driver](../specs/design-udf-driver.md)
designs are the contract. The plan's stages are the work; these items are
the questions and follow-ups that outlive a single stage.

* [High] **Standards not confirmed against the published text**
  (`specs/design-udf.md`, `specs/design-udf-driver.md`): every rule marked E1
  or U1 comes from the draft, the NetBSD offsets and observation, because
  the environment that wrote the designs could not reach the ECMA documents.
  Read ECMA-167 and TR/112 parts 2 to 7 and confirm or correct each rule
  (research question Q1, plan stage 1).
* [Medium] **No independent metadata-partition image**
  (`docs/udf-research.md`): mkudffs writes a VAT for its 2.50 `bdr` profile,
  so no Linux tool produces a metadata partition. A NetBSD `makefs` or
  `newfs_udf` image, a Windows 2.50 volume or a BD-ROM structure survey is
  needed before the metadata stage lands (Q12).
* [Medium] **Whole-disk UDF behind an MBR** (`lib/layers/mbr.c`,
  `specs/design-layers.md`): `mkudffs --bootarea=mbr` records one MBR entry
  starting at LBA 0 and spanning the disk, which the MBR probe claims and
  validation rejects, so such a disk reads only with the scheme forced
  empty. Decide whether the MBR probe declines such an entry (Q16).
* [Medium] **Partition types for UDF volumes** (`specs/design-layers.md`):
  the layer design's GPT and MBR type tables have no `udf` row, so a scheme
  around a UDF volume has no derived type. mkudffs uses MBR type `0x07`;
  record what Windows and NetBSD assign before the writer composes with a
  scheme.
* [Low] **UDF 1.02 output** (`lib/formats/udf/write-plan.c`): a `revision=`
  setting for 1.02 with file entries and the file times attribute for
  creation time, for players and systems that read only 1.02, behind the
  same native qualification as 2.01.
* [Low] **Read-only writer profile** (`lib/formats/udf/write-plan.c`): an
  access-type-1 partition without a space bitmap, as DVD-ROM masters carry,
  with its own qualification.
* [Low] **Metadata-partition writer** (`lib/formats/udf/`): a 2.50 profile
  that moves the writer's first allocation region into a metadata file and
  mirror, gated by NetBSD `fsck_udf` and native mounts.
