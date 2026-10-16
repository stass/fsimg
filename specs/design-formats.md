# Filesystem format contracts

## Format descriptors and profiles

A format descriptor names its on-disk family, probe, independent reader/writer
operations, geometry constraints, supported entry/metadata features, and
verification scope. Registration is static. Shared helpers handle bounded I/O,
endian conversion, UTF validation, resources, and source copying; a driver does
not acquire host descriptors or allocate outside its context.

The public names are `fat12`, `fat16`, `fat32`, and `exfat`. Detection identifies
the actual format from validated structures. A forced name selects a decoder,
not permission to skip consistency checks. Report a recognized unsupported
variant separately from an unrecognized image or a corrupt supported variant.

The default writer profile emphasizes interoperability: a 512-byte sector
default, documented legal alternative sector sizes, conservative FAT allocation
units, and explicit limits. All accepted geometry combinations are listed in
the driver's capability table and independently tested. Larger exFAT allocation
units are streamed in pieces; their size never dictates a payload-buffer size.
Format-theoretical limits and fsimg's supported profile are reported separately.

Every generated image has an entry in a fixture manifest recording format,
geometry, feature profile, source provenance, creation command, and independent
validator results. Imported fixtures and generated Unicode/codepage tables have
license/provenance records. External format tools are validation dependencies,
not code-vendoring authorization or runtime dependencies.

## FAT12, FAT16, and FAT32

Use Microsoft's FAT specification as the on-disk reference, including its
cluster-count classification rather than the volume label: fewer than 4,085
data clusters is FAT12; fewer than 65,525 is FAT16; otherwise the supported
FAT32 range applies. A regular file's size field is 32 bits, so sizes above
`UINT32_MAX` are rejected. These rules and the layout details are in the
[Microsoft FAT specification](https://www.scs.stanford.edu/~zyedidia/docs/_other/fat.pdf)
(Microsoft-authored document, hosted by Stanford).

The reader validates boot geometry, table extents, usable/reserved cluster
values, directory records, long-name sequences, and every followed chain.
FAT12 entries crossing a cache-page boundary are decoded without unaligned
loads. FAT32 uses the relevant bits and declared active/mirrored table policy;
it does not silently choose an arbitrary table when copies disagree.
Full verification checks copy consistency and allocation ownership according
to the declared policy; normal reads report their narrower scope.

The writer owns variant-specific root-directory handling, long-name slot
counts, deterministic short aliases, reserved entries, table copies, and FAT32
backup/FSInfo structures. Table generation is streamed from allocation runs.
Root capacity and directory record limits can fail independently of free data
space. Geometry near type thresholds is tested on both sides, and automatic
sizing respects the requested variant rather than changing it.

Long names preserve validated UTF-8 through UTF-16 encoding; counts are in
encoded units, not C characters. Short-name-only input uses an explicit codepage
profile, with CP437 as the supported default rather than the host locale.
Unsupported codepages fail explicitly. Case-equivalence, forbidden characters,
long-name/alias cross-collisions, and timestamp interpretation belong to the
FAT codec. Its versioned mapping tables and compatibility corpus are part of
the supported profile; no locale-dependent `toupper()` or implicit Unicode
normalization decides image names.

FAT timestamps have limited range/resolution and no universal UTC offset.
The writer uses a documented UTC-based encoding policy independent of the host
timezone; reproducible mode uses the selected epoch rounded to representable
precision. Reading exposes the original wall-clock value with unknown-zone
semantics rather than inventing a local timezone. Required unrepresentable
timestamps fail; optional rounding/loss is reported by policy.

Interoperability gates include mtools extraction and `fsck.fat` inspection,
images created/populated by independent tools, and an OS reader on isolated
test infrastructure. Tests cover all variants, fixed-root exhaustion, threshold
geometry, long-name/alias collisions, Unicode, deleted records, fragmentation,
cycles, empty files, full volumes, and the maximum file-size boundary. Host
mounting of hostile fixtures is not an ordinary developer test.

## exFAT

The normative reference is Microsoft's
[exFAT specification](https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification).
The supported profile covers ordinary non-TexFAT volumes with one FAT, normal
file/directory entry sets, allocation bitmap, upcase table, contiguous and
chained content, and 64-bit lengths constrained by the host adapter. TexFAT
and unknown critical features are unsupported, not guessed.

Readers validate both boot-region integrity and geometry, required metadata,
entry-set checksums, name hashes, UTF-16, upcase decoding, bitmap bounds, and
stream extents. `ValidDataLength` must not exceed `DataLength`; the logical
tail after valid data is returned as zero without reading stale cluster bytes.
Contiguous streams validate their complete range, and chained streams obey
bounded traversal. Root traversal does not assume the contiguous-file rule.

Writers stream allocation metadata, emit complete checksummed boot regions and
directory sets, and use a versioned deterministic upcase table. They set
contiguous-file flags only when warranted. Every cluster allocation, directory
length, name length, and checksum follows the selected profile. Timestamp
offsets are interpreted explicitly; an unknown offset remains unknown.

The upcase table is a format-bounded object, but its decoded storage still
counts against the budget. Allocation bitmaps and FATs are never mandatory
whole-volume allocations in RAM. Large zero sources can have a valid logical
length without materializing their content in memory or scratch payload files.
The writer still reserves the format-required allocation and emits its
metadata; a sparse host representation does not change ownership on disk.

Independent exfatprogs validation, externally populated volumes, and OS-reader
checks gate support. Fixtures cover >4 GiB files, initialized/zero boundaries,
fragmentation, corrupt metadata checksums, malformed entry sets, upcase/name
behavior, large allocation units, and wide directories within the format limit.
The [scale acceptance cases](architecture.md#acceptance-contract) test gigantic
logical volumes independently from large entry and fragment counts.

## Verification and compatibility claims

`info` describes recognized geometry and implemented capabilities. `verify`
performs complete structural checks for the supported profile and optionally
reads payloads. Neither performs filesystem repair or claims that arbitrary
unknown extensions are safe to ignore. Unknown benign records are skipped
only where the format expressly defines that behavior and their lengths can
be validated.

A driver's read-only implementation does not need writer stubs. Adding a
filesystem requires a design that accounts for geometry, feature negotiation,
ownership, names, traversal termination, memory/scratch complexity, hostile
inputs, and independent validation. NTFS and the distinct QNX4/QNX6 formats
are reader-first extensions; no broad compatibility claim is made merely from
parsing a boot record. Their concrete profiles belong in separate designs.
