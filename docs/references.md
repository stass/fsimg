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

The generated `lib/formats/hfsplus/unicode-tables.h` uses Unicode-3.0 data under
the same notice, and project-owned exceptions. Its inputs are:

| Data | Source | SHA-256 |
| --- | --- | --- |
| UnicodeData 3.2.0, combining class, canonical decomposition and simple lowercase fields | [Unicode Consortium](https://www.unicode.org/Public/3.2-Update/UnicodeData-3.2.0.txt) | `5e444028b6e76d96f9dc509609c5e3222bf609056f35e5fcde7e6fb8a58cd446` |
| HFS Plus fold exceptions, each row naming its observation | `tools/hfsplus-unicode-exceptions.txt` | `b7317b3da963c6e6b02fc188b56a17302ff1bbd3ec2374044119cc3094bb9319` |

Regenerate with:

```sh
ruby tools/gen-hfsplus-tables.rb UnicodeData-3.2.0.txt \
    tools/hfsplus-unicode-exceptions.txt lib/formats/hfsplus/unicode-tables.h
```

The generator rejects other inputs and emits both digests. Changing an exceptions
row means changing the digest pinned in the generator as well.
`tools/hfsplus/check-unicode-tables.rb` checks the header by hand against the E3
captures in [the research record](hfsplus-research.md), and against
[NetBSD `sys/fs/hfs/libhfs.c`](https://raw.githubusercontent.com/NetBSD/src/e4f73ac629cd72caf75fdbe9eac3eeaafeaca1f0/sys/fs/hfs/libhfs.c)
(BSD-2-Clause, revision 1.19, GitHub mirror commit
`e4f73ac629cd72caf75fdbe9eac3eeaafeaca1f0`, SHA-256
`d4aeef80a6b06232340182da3d74bc91e03bfc39de084713eac2d714f10d7f94`), whose folding
table is compared, never imported: no NetBSD text is in the tree.

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

## LZFSE and LZVN decoders

`lib/codec/lzfse/` holds the decoder half of Apple's reference implementation,
<https://github.com/lzfse/lzfse>, under its BSD-3-Clause license, at commit
`e634ca58b4821d9f3d560cdc6df5dec02ffc93fd`. Each
file's header was checked before import and each carries that notice;
`third-party/lzfse-LICENSE.txt` repeats it for binary distributions. Watch that
repository's issues and any Apple security advisory naming `lzfse`: an upstream
fix is imported as a new pinned commit and audited again, file by file. Apple's
xnu and hfs sources are APSL and contribute facts only, never code; this project
is separate from them.

| File | SHA-256 of the upstream file |
| --- | --- |
| `src/lzfse_decode_base.c` | `721b4061e4e655f0940b7cabcf506356c152716526e630981caf0e708d2f73d9` |
| `src/lzfse_fse.c` | `a97e309975487af269456a4e87ef516aedaf82ce469e37971fd5676f9cd5956b` |
| `src/lzfse_fse.h` | `4e5c2228a4bb5e5f3faf4639d5688bc7caa5cf0c0f3a971ada4fbe3631f3a533` |
| `src/lzfse_internal.h` | `d08dc29d5440775de279d36783729131329df10cffd43ec6eff7dcfceef9f570` |
| `src/lzfse_tunables.h` | `99d435ca45b32ba0cc273a2862ba714762610513eed469bc590bb092ea476b36` |
| `src/lzvn_decode_base.c` | `434647c83d4f3ed95c11465c5a3ec46f992ec94843928138039ea40be623957d` |
| `src/lzvn_decode_base.h` | `d4c13bbf13b3616c1b1472e80b117857f1339a29a48006e5aad450bcd6204c09` |
| `LICENSE` | `2a20082c2219edbbdb2e862a0566ba4f762a564ade7c04b09250ae4464bc4b32` |

`lzfse_decode.c` and `lzfse.h`, the buffer interface with its allocation path, are
not imported: the wrapper, `lib/codec/lzfse.c`, calls the two state machines
directly. Neither are the encoder files or the command-line tool. All the
upstream files are ASCII. Every change below is marked `fsimg:` in place, except
the last two:

* `lzfse_internal.h`: the external symbols take the `fsimg_apple_` prefix through
  four `#define` lines.
* `lzfse_fse.c`: the encoder table and frequency normalization are removed.
* `lzfse_fse.h`, a defect: `fse_in_checked_flush64()` always loads eight bytes, so
  with nothing to load after an initial stream of `n == 0` it reads one byte past
  the payload, which a hostile block can place at the end of the input. It
  returns first when there is nothing to load.
* `lzfse_fse.h`, a defect: both `fse_in_checked_init` functions refuse an `n`
  outside `[-7, 0]` before adding to it, since a v1 block header gives `n`
  unchecked and the sum could overflow.
* `lzfse_fse.h`, a defect: `fse_in_checked_flush32()` loads through `memcpy`
  instead of a misaligned `uint32_t` dereference. This path runs on hosts that do
  not select the 64-bit stream, including Linux on arm64.
* `lzfse_decode_base.c`, a defect on a 32-bit host: the block header's checks
  against the end of the input compare sizes, because a pointer plus a 32-bit
  field from the stream can wrap.
* `lzvn_decode_base.c`, a defect: `sml_m` and `lrg_m` refuse a distance of zero,
  which only a stream with no earlier match can have. They use the previous
  distance without the check the other opcodes make, so such a match copied each
  output byte onto itself and passed whatever the buffer held as decoded data.
* `lzfse_decode_base.c` and `lzfse_internal.h`, a resource bound: a stream fails
  once its blocks declare more than `4 * (capacity + 40000)` of `n_literals + 3 *
  n_matches`. A block's decode cost follows those counts, not its payload: a
  39-byte block with no tables of its own can decode 40000 literals and 10000
  matches that take no input bits and make no output, about 6 s of CPU per MiB of
  image. The limit bounds that cost; it does not make every accepted stream
  produce output, because a stream of no-output matches under the limit still
  decodes. For the streams Apple's encoder writes the limit is far from binding:
  102 native chunks and 1780 plaintexts through `compression_encode_buffer` peak
  at 15.67% of the bound.
* `lzfse_fse.h`: `fse_check_freq()`'s loop index is `size_t`, because the wrapper
  includes this header under the project's warnings.
* The build, `libfsimg-lzfse.la` in `Makefile.am`: `-Wno-sign-compare` and
  `-Wno-unused-label`, for the upstream code's own comparisons and the labels of
  the computed goto it does not use here; `-DHAVE_LABELS_AS_VALUES=0`, its switch
  in place of the computed goto, which is not C11; and `-DNDEBUG`, because the
  library must not abort. The assertions state invariants the reading of these
  paths found to hold on hostile input.

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

## ISO 9660 references

[ECMA-119](https://ecma-international.org/publications-and-standards/standards/ecma-119/),
sixth edition, with its Annex C for Joliet, is the normative volume and
file structure reference. The IEEE P1281 (SUSP) and P1282 (RRIP) drafts at
version 1.12 and the El Torito 1.0 specification define the system-use
protocol, Rock Ridge and boot catalogs. These documents are copyrighted:
the [format design](../specs/design-iso9660.md) restates their layouts in
project prose and copies no text, and the research draft
[fsimg_iso9660_spec.md](../fsspecs/fsimg_iso9660_spec.md) records the links.

libarchive's ISO reader and writer (BSD-2-Clause) and FreeBSD's `cd9660`
and `makefs` (BSD) are behavioural references and oracles; no code from
them is imported. `xorriso` and libisofs, `genisoimage`, `isoinfo` and
`isovfy`, and `pycdlib` (GPL and LGPL families) are external programs used
only as producers and consumers of test images, at the versions
[build.md](build.md) records, and never enter the repository or the link.
Test images are generated from manifests; none is committed.
