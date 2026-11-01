# Implementation references

The FAT reader and writer use the author's libarchive msdosfs implementations
as sample code:

* `~/libarchive/libarchive/archive_read_support_format_msdosfs.c`
* `~/libarchive/libarchive/archive_write_set_format_msdosfs.c`

The FAT code is Stanislav Sedov's own work and needs no separate notice. The
reader and writer adapt format logic from these implementations; their
libarchive registration and callback interfaces are adapted to fsimg's
independent reader and writer contracts. libarchive is not a core runtime
dependency.

The reader's BPB parsing, FAT12 packed-entry decoding, chain handling, long-name
checksum, and timestamp decoding provide format examples. The writer's geometry
selection, boot records, FSInfo, FAT emission, and directory-entry encoding
provide emission examples. These examples are checked against the format
specification and independent images as part of FAT acceptance.

fsimg's resource design governs adaptation: allocation tables, complete
catalogs, wide directories, and fragment indexes use bounded storage; file
payloads are copied from repeatable sources directly to their planned offsets.
The sample reader retains a FAT and complete entry catalog in memory, and the
sample writer stages payloads in a temporary file. Those storage choices are
replaced by fsimg's specified algorithms when adapting the format logic.

`lib/formats/fat/codec.c` and `lib/formats/fat/read.c` adapt BPB field
interpretation, packed FAT examples, and the long-name checksum from the reader
above. Resource storage, traversal, collision handling, calendar conversion, indexing,
and verification are project-owned implementations.

`lib/formats/fat/write-emit.c` adapts boot/FSInfo fields and long-name slot
encoding from the writer above. Geometry search, alias reservation, paged plans,
calendar conversion, and direct source emission are project-owned implementations.

The generated `lib/formats/fat/name-tables.h` and `lib/core/case-tables.h`
use Unicode-3.0 data, with the
full permission notice in `third-party/Unicode-LICENSE.txt`. Both notices are
installed with the library documentation. Its inputs are:

| Data | Source | SHA-256 |
| --- | --- | --- |
| UnicodeData 17.0.0, simple uppercase/lowercase fields | [Unicode Consortium](https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt) | `2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c` |
| CP437 mapping 2.00 | [Unicode mapping archive](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/CP437.TXT) | `6bad4dabcdf5940227c7d81fab130dcb18a77850b5d79de28b5dc4e047b0aaac` |

Regenerate from the unmodified downloaded inputs with:

```sh
ruby tools/gen-fat-tables.rb UnicodeData.txt CP437.TXT \
    lib/formats/fat/name-tables.h lib/core/case-tables.h
```

The generator rejects source hashes outside the pinned inputs, preserves ASCII
source, and emits source hashes. The mapping's
DOS end-of-file byte is excluded from records, and included in its source hash.
No locale or host Unicode library supplies comparison behavior at runtime.

`lib/core/sha256.c` and `lib/core/sha256.h`, the project's one SHA-256
implementation, are FreeBSD's `sys/crypto/sha2/sha256c.c` and
`sys/crypto/sha2/sha256.h` by Colin Percival, under a BSD-2-Clause notice,
taken from the FreeBSD src repository at commit
`975e3605ebb15cbaf5a25c1c9d1f51aed41291d0`:

| File | SHA-256 of the upstream file |
| --- | --- |
| `sys/crypto/sha2/sha256c.c` | `9eed9976f45476f7dd0dd452861416ff1f45b2a7035909171dc6fef836ba9b61` |
| `sys/crypto/sha2/sha256.h` | `10f3fd28a59056ad4701c58570d55cd4de4dcebfbaa490c154b7005f8e16dabe` |

Both files retain the notice, which
`third-party/FreeBSD-sha256-LICENSE.txt` repeats for binary distributions,
and list their adaptations in a comment after it: the SHA-224 variant, the
arm64 instruction dispatch and the libmd glue are removed, the names take the
`fsimg_` prefix, portable byte-order helpers replace `<sys/endian.h>`, an
update's byte count is widened before it is converted to bits, an empty
update returns before `memcpy`, and `bzero` replaces `explicit_bzero`.

`test/fat.c` owns BSD-2-Clause synthetic fixture generation. The fixed layouts,
geometry matrix, corruption mutations, and content patterns are encoded
independently of production decoders. `test/fat-integration.sh.in` creates
external fixtures using dosfstools or native FreeBSD FAT tools, plus mtools,
without copying their code or
shipping their generated images. Its test log is the fixture manifest: tool
versions, creation/population commands, geometry, fragmentation recipe, and
independent validator/fsimg outcomes. These are development tools only.

The normative FAT reference is the [Microsoft FAT specification](https://www.scs.stanford.edu/~zyedidia/docs/_other/fat.pdf),
including page 15's cluster-count classification and formatting margin.
FreeBSD's `newfs_msdos`/`fsck_msdosfs`, dosfstools and mtools are independent
development oracles, not runtime dependencies or imported implementation code.

The additional [fatcat](https://github.com/gregwar/fatcat) oracle is built from
revision `76000303082e9e86f1aca8edb030c5fb8a63ae55` by
`build-aux/build-fatcat.sh`. The external checkout retains its MIT license.
`build-aux/fatcat-freebsd.patch` selects the existing native getopt/64-bit-offset
interfaces on FreeBSD and changes no on-disk interpretation. No fatcat source
is incorporated in fsimg. Its pinned reader requires 512-byte sectors and has
limited non-ASCII name decoding; comparisons use ASCII names and ASCII long
names, while mtools validates Unicode. Its information output is recorded as
tool output, not used as the oracle for free-space or cluster counts.

The specification-driven harness under `test/spec/` writes its rules, layouts
and builder from the Microsoft specification, independently of
`lib/formats/fat/` and `test/fat.c`, and uses the oracles above on the same
terms. Its development gems, pinned in `test/spec/Gemfile.lock`, are Minitest
(MIT) with its dependencies drb (BSD-2-Clause) and prism (MIT), PropCheck (MIT)
and BinData (BSD-2-Clause); none is vendored or becomes a runtime dependency.

## exFAT reference

The [Microsoft exFAT specification](https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification)
is the normative on-disk reference. exfatprogs is an independent development
oracle; its GPL implementation is not imported into fsimg. The writer uses
the pinned Unicode simple-uppercase mapping for its complete BMP upcase table.
Readers use the checksum-validated table embedded in each volume.

## QNX6 references

The [QNX6 evidence record](qnx6-research.md) identifies pinned Linux, NFI,
ReFirmLabs, and Porsche PCM-Explorer references, vendor documentation, license
boundaries, and independently checked fixture observations. These sources
inform the [QNX6 design](../specs/design-qnx6.md); no third-party code or
binary fixture is imported. Read-only parsers are not formatter oracles.

## UDF references

ECMA-167 3rd edition and the ECMA TR/112 republication of UDF 1.02 to 2.60
are the normative references for the [UDF design](../specs/design-udf.md);
they are public standards, paraphrased, never copied. NetBSD's BSD-licensed
`ecma167-udf.h` cross-checks field offsets; no NetBSD code is adapted.
udftools, genisoimage, pycdlib, 7-Zip and the Linux driver are GPL or LGPL
black-box oracles, and the NetBSD tools, Windows and macOS are run as
external programs. The [UDF evidence record](udf-research.md) pins their
versions and records what each image showed. No third-party code, captured
image or disc survey enters the repository.
