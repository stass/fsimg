# exFAT reader and writer

## Profile and ownership

The normative reference is the [Microsoft exFAT specification](https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification).
The driver supports ordinary volumes with one FAT, sectors from 512 through
4096 bytes, allocation units from one sector through 32 MiB, and file/directory
entry sets with UTF-16 names. Two-FAT TexFAT volumes and unknown critical
features fail explicitly. Formats are statically registered; readers and
writers have independent operations and independent format state. Common
reader lifecycle, source catalog, resource context, selections, extraction,
and publication remain their existing owners. No mutable filesystem exists.

All on-disk sizes use checked volume-relative arithmetic before common I/O.
Cluster identifiers are validated against the decoded heap rather than FAT32
masks. A geometry cannot authorize bytes beyond the input slice or host offset
range. A forced format still validates the complete supported header.

## Concrete directory entry layouts

Critical entries redefine fields of the generic primary template. Their
concrete layouts, rather than an assumed common header, govern decoding:

| Entry | Relevant byte offsets within its 32-byte record |
| --- | --- |
| File, Table 27 | Attributes 4; creation/modification/access times 8/12/16; subsecond increments 20/21; UTC offsets 22/23/24 |
| Allocation bitmap, Table 20 | Bitmap selector flags 1; first cluster 20; data length 24 |
| Upcase, Table 23 | Encoded table checksum 4; first cluster 20; data length 24 |
| Volume label, Table 26 | UTF-16 unit count 1; label units 2; reserved tail 24 |

File sets use the checksum at bytes 2–3 across their primary and secondary
records. Bitmap, upcase and label records have no entry-set checksum field.
The upcase record instead checksums its encoded table content. The generic
override rules are in sections 6.3.2–6.3.4 of the normative reference.
Unrecognized benign primaries follow generic primary flags at bytes 4–5;
benign secondaries, including Vendor Allocation (Table 37), use flags at byte 1.
Both templates place FirstCluster at byte 20 and DataLength at byte 24.

## Opening and metadata discovery

The reader checks both twelve-sector boot regions with bounded sector buffers.
It validates checksums before trusting geometry, boot/extended signatures,
mandatory-zero bytes, sector/cluster shifts, volume minimum/length, FAT capacity,
heap ordering and cluster-count derivation. The backup geometry agrees with the
main geometry; stale backup flags and percentage do not override main values.
Recognized unsupported features remain distinguishable from malformed data.
Table 4 places DriveSelect at byte 111 and PercentInUse at byte 112. Boot
checksums exclude bytes 106, 107 and 112; DriveSelect remains covered.

Root allocation is always chained. A bounded bootstrap root pass locates exactly
one bitmap and upcase entry plus the optional label. Missing, duplicate,
misplaced or invalid metadata fails. Metadata chain indexes use the common
paged store and finite chain steps, so a fragmented bitmap is not a resident
extent vector. Bootstrapping never trusts file names before validating upcase.

The bitmap owns allocation state; FAT zero entries do not imply free space for
contiguous streams. Bitmap length covers at least the heap's cluster bits; trailing reserved bits
are permitted. The declared length and its full allocation remain bounded.
The upcase checksum covers its encoded bytes. Encoded input is streamed under
session quotas; it is not allocated according to an untrusted table length. Decoding accepts compressed and
uncompressed tables with exactly 65,536 mappings, validates identity-run bounds
and mandatory ASCII mappings, and charges the decoded 128 KiB table. A nonterminal FFFF word introduces an identity-run count, including an empty
run; the terminal mapping for U+FFFF is literal. Encoded length does not select
a separate table mode. Reader
name comparison always uses that volume's table. Writer names use a pinned
Unicode simple-uppercase BMP mapping; surrogate units remain unchanged. This
is a legal custom complete table, identified in the writer profile. Table data
comes from the existing Unicode-licensed mapping source, not GPL test tools.
The writer compresses identity runs of at least three mappings. It computes
the encoded length and checksum from the same owned byte sequence it emits;
shorter identity runs and non-identity mappings remain literal words.
The reader profile `embedded-upcase` describes volume-owned comparison data;
the registry profile `embedded-upcase/unicode-17-writer` also identifies the
writer's pinned table source.

## Traversal, content and verification

A stream describes first cluster, logical length, valid-data length and either
contiguous allocation or an explicit chain. Geometry bounds contiguous ranges.
Chains have both cycle detection and a heap-size step ceiling, validate bitmap
allocation and reserved/bad values, and require sufficient coverage for the declared logical
length. A chained regular file can own additional allocation, including when its
logical length is zero. Its entire chain is validated and owned; extra clusters
never become readable payload. A directory stream describes its entire
allocation, so its length equals that allocation. The root has no stream length;
its chain is bounded by heap geometry and session quotas rather than imposing
the non-root stream field's 256 MiB limit. Cluster size never dictates a
transfer-buffer size.

Common [logical content](design-content.md) delivery owns reads and ranges.
The exFAT initialized length is the valid-data length, which never exceeds the
logical length. The logical tail is ZERO and its backing clusters are never read.
The driver validates each regular file's allocation when the entry is exposed,
so listing reports corrupt file allocation. That pass builds the shared extent
index used to resolve offsets. Allocated clusters beyond valid
data remain owned and are structurally checked. Payload verification visits
only logical file bytes and never treats uninitialized physical storage as user
data.

Directories follow the [prepared-directory contract](design-directories.md).
Each entry's namespace key is its name mapped through the validated upcase
table; record payloads carry the decoded stream fields as the private locator.
Preparation validates complete entry-set checksums
before interpreting secondary fields, stream/name entry order, name-unit counts,
hash, strict UTF-16, legal components, attributes, timestamps, and allocation
length. Unknown benign
entries are skipped only according to their template, with associated allocation
validated and retained for ownership checks. Unknown critical sets are unsupported.
Deleted entries and the end marker cannot interrupt a live entry set. Directory
allocation is checked even after the first terminator. A paged directory-owner
bitset rejects cycles and sharing independently of depth limits.

Full verification records metadata, directory, contiguous-file and chain extents,
sorts ownership runs externally and merges them with a sequential bitmap/FAT
scan. Overlaps, unallocated owners, invalid chained allocation and orphan allocations
fail. Allocated unowned clusters explicitly marked bad in the FAT remain
accounted bad storage rather than orphan file data. FAT values for NoFatChain allocations are ignored as the format requires.
Clean/media status and boot integrity remain separate from successful opening.

## Planning and emission

The writer validates names through its pinned table, externally checks sibling
keys, computes directory entry-set lengths and timestamps, and stores immutable
paged layout rows by catalog ID. No short aliases exist. Directory lengths
include a zero terminator and whole-cluster padding, bounded by 256 MiB. Labels
contain at most eleven UTF-16 units; their UTF-8 backing is copied and bounded
independently of FAT's eleven-byte label. The common API's NULL label selects
NO NAME; an explicit empty label records zero units. The volume serial is used
verbatim, including its default zero. UTC/offset timestamps use explicit
quarter-hour offsets and UTC instants; unknown zones preserve wall-clock
semantics. A known zero offset has canonical UTC basis on reading. Volume
information reports PER_TIMESTAMP: each timestamp
carries its own basis, including when fields within an entry differ.
Unrepresentable required fields fail; optional loss is counted and
reported through the common [writer policy](design-metadata-loss.md).
Reproducible mode selects its fixed epoch.
Absent or unrepresentable optional timestamps use 1980-01-01 00:00:00 UTC.
Fallback replaces the complete timestamp, including its fractional part.

Geometry selection is finite and deterministic. Automatic sizing satisfies the
one-MiB minimum and free-space reserve. Exact sizing derives a sufficient FAT
and heap cluster count with checked arithmetic. Automatic large-volume choices
prefer no more than the specification's recommended cluster count when a legal
allocation-unit size permits it; explicit geometry retains its explicit work
cost. Bitmap and upcase storage participate in allocation accounting.

Allocation assigns deterministic contiguous ranges to metadata and catalog
objects. Root and mandatory metadata use FAT chains. Ordinary streams can use
NoFatChain only when their layout proves contiguous nonempty allocation.
FAT, bitmap, directory sets and upcase bytes stream directly from the plan in
bounded buffers. All reserved bytes, boot padding, directory slack, free space
and unused initialized ranges are defined. Fresh sparse sinks keep known zero
ranges without writing their logical capacity. Reused sinks use the common
explicit initialization rules. Plan initialized_bytes counts the format-initialized payload extent: FAT
uses logical file lengths, while an exFAT zero source has valid-data length zero
and contributes zero initialized bytes. Other source kinds can have initialized
logical zero ranges. This count is separate from actual I/O transfer.
Pre-mutation checks, initialization, and payload copying follow the common
[emission contract](design-emission.md); the output coordinator owns
publication.

## Validation and scale

Independent fixtures and fsck use exfatprogs; imported tools are development
oracles, not runtime dependencies or source provenance. Synthetic independent
codecs construct malformed boot checksums, name hashes, upcase compression,
bitmap bits, entry sets, lengths and fragmented allocation. Poisoned bytes beyond
valid-data length must be zero through read_at, range iteration, CAT and host
extraction. Tests compare displayed Unicode names, supported metadata, bytes,
reproducibility, allocation failures, short I/O and cleanup under sanitizers.

The 128 TiB qualification uses a 64 MiB library budget with a multi-terabyte
zero source and nonzero boundary markers. Logical capacity, initialized bytes,
actual metadata/payload transfer, charged allocations, scratch, descriptors,
RSS and elapsed time are separate measurements. A sparse callback/FD fixture
does not claim full multi-terabyte transfer or support beyond host file limits.
EOF, malformed-image and fuzz inputs always have finite budgets.
