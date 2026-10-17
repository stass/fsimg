# Implementation references

The FAT reader and writer use the author's libarchive msdosfs implementations
as sample code:

* `~/libarchive/libarchive/archive_read_support_format_msdosfs.c`
* `~/libarchive/libarchive/archive_write_set_format_msdosfs.c`

Both files carry Stanislav Sedov's BSD-2-Clause notices. Code reused from them
retains their copyright, conditions, and disclaimer. Their libarchive
registration and callback interfaces are adapted to fsimg's independent reader
and writer contracts; libarchive is not a core runtime dependency.

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
above. Its BSD-2-Clause notice is retained in both files and in
`third-party/libarchive-msdosfs-LICENSE.txt` for
binary distributions. Resource storage, traversal, collision handling, calendar
conversion, indexing, and verification are project-owned implementations.

The generated `lib/formats/fat/name-tables.h` uses Unicode-3.0 data, with the
full permission notice in `third-party/Unicode-LICENSE.txt`. Both notices are
installed with the library documentation. Its inputs are:

| Data | Source | SHA-256 |
| --- | --- | --- |
| UnicodeData 17.0.0, simple uppercase/lowercase fields | [Unicode Consortium](https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt) | `2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c` |
| CP437 mapping 2.00 | [Unicode mapping archive](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/CP437.TXT) | `6bad4dabcdf5940227c7d81fab130dcb18a77850b5d79de28b5dc4e047b0aaac` |

Regenerate from the unmodified downloaded inputs with:

```sh
python3 tools/gen-fat-tables.py UnicodeData.txt CP437.TXT lib/formats/fat/name-tables.h
```

The generator rejects source hashes outside the pinned inputs, preserves ASCII
source, and emits source hashes. The mapping's
DOS end-of-file byte is excluded from records, and included in its source hash.
No locale or host Unicode library supplies comparison behavior at runtime.

`test/fat.c` owns BSD-2-Clause synthetic fixture generation. The fixed layouts,
geometry matrix, corruption mutations, and content patterns are encoded
independently of production decoders. `test/fat-integration.sh.in` creates
external fixtures using dosfstools and mtools without copying their code or
shipping their generated images. Its test log is the fixture manifest: tool
versions, creation/population commands, geometry, fragmentation recipe, and
independent validator/fsimg outcomes. These are development tools only.
