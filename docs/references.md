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
