# FAT writer

The FAT writer consumes the sealed common catalog. It does not reuse reader
state, retain payloads, or acquire host paths. Its immutable plan borrows the
writer and owns a paged fixed-record array indexed by catalog ID, including
ID zero for the implicit root. Records contain directory slot counts, a
child's slot within its parent, encoded short metadata, and a contiguous
cluster allocation. All bytes in scratch records are explicitly encoded.

## Names and directory layout

The name and comparison profile matches the FAT reader: validated UTF-8,
at most 255 UTF-16 units, CP437 short names, and pinned simple uppercase
comparison. An external sort of `(parent ID, effective name key)` rejects
duplicate and case-equivalent siblings before allocating an image.

Exact uppercase ASCII 8.3 names use their natural short name. Other names use
long-name records and an ASCII alias `Fhhhhhhh`, with seven uppercase hexadecimal
digits. Each directory assigns increasing numbers in catalog child order.
Every effective name in this alias family reserves its number, including names
that need a long-name record. A sorted reservation stream is merged with child
order; the next number skips reserved values. Thus aliases cannot collide with
any effective name or other alias. Assignment needs no repeated directory
search and is independent of memory budget and insertion order. The finite
directory slot limit bounds the number of reservations and assigned aliases.

Directory sizes include long-name slots, short entries, a zero terminator,
the root volume label, and non-root `.` / `..` entries. Directories cannot
exceed 2 MiB. FAT12/16 roots use a sector-aligned fixed region, selected to fit
the entries (at least 512 slots) unless explicitly constrained. FAT32 roots and
all other directories occupy ordinary contiguous cluster chains. The parent
cluster for `..` is zero when the parent is the volume root.

## Geometry and allocation

Legal sectors are 512, the default, 1024, 2048, or 4096 bytes. The
interoperable writer profile limits clusters to powers of two between the
sector size and 32 KiB.
Two identical FAT copies are emitted. FAT12/16 reserve one sector; FAT32
reserves 32, uses FSInfo sector 1 and backup boot/FSInfo sectors 6/7.

Candidate cluster sizes are tried in increasing order, or all evaluated under
the [minimization option](design-sizing.md). One catalog-array pass counts
directory/file clusters for every candidate. Automatic sizing uses
the greater of its interoperability minimum and allocated clusters plus the
requested free reserve, then computes exact FAT and volume lengths. Automatic
counts stay at least 16 clusters away from the 4085/65525 type cutovers,
following Microsoft's formatting recommendation. A candidate too close to the
upper cutover gives way to the next permitted cluster size. Exact sizing retains
the complete legal variant range, including those boundary counts, and uses a
bounded binary search for the smallest FAT sector count whose capacity covers
the resulting data area. Cluster-count classification, legal cluster IDs,
32-bit sector/file fields, root capacity, and free reserve must all hold.
There is no convergence loop or image-wide search per entry. Allocation follows
catalog ID order, starting with the FAT32 root; empty files have no cluster.

Candidate rejection distinguishes a volume below or above the variant's cluster
range, catalog/reserve capacity, insufficient data space and on-disk field
limits. A failed search reports a specific cause only when every candidate
agrees. Mixed rejection causes report conflicting requested settings. Diagnostics
reference an explicit image size or a nonzero requested free-space reserve only
where relevant. Automatic sizing is never presented as a zero-byte image.
Without a reserve, a catalog exceeding cluster capacity is described as a
source tree too large for the selected variant; an explicit cluster size
qualifies that limit. Diagnostics never
claim an uncomputed minimum size or a remedy not established by planning.

## Metadata and determinism

Missing timestamps use 1980-01-01. Timestamp conversion uses integer Gregorian
calendar arithmetic, independent of locale, timezone, and libc time functions.
UTC/offset instants encode UTC calendar fields; wall times preserve their
calendar fields. Creation precision is 10 ms, modification 2 seconds, and access
one day. Values outside 1980 through 2107 are omitted in favor of the default.
Rounding, time-basis/offset loss, and reproducible epoch substitution are losses.
Required losses fail. Optional losses require `FSIMG_WRITE_ALLOW_LOSS`; the
initializer selects this explicit policy. The common
[writer loss operation](design-metadata-loss.md) counts affected metadata
fields and reports them through the context's optional synchronous callback.
The driver supplies fidelity results; successful planning publishes the total.
Reproducible mode uses its selected epoch for all emitted times. The volume
serial is explicit and defaults to zero. The default label is `NO NAME`;
explicit labels are uppercase ASCII FAT labels of at most eleven bytes.

## Emission and publication

Planning is single-use and repeatable plan queries return the same borrowed
object. The common [emission contract](design-emission.md) owns pre-mutation
checks, sink initialization, payload copying, and failure marking. The FAT
emitter writes the boot sectors, streams FAT entries from the contiguous
allocation records, encodes directory records directly at planned offsets, and
requests one payload copy per file into its planned cluster range.

`write_at` uses the common output coordinator to create a private staging
directory and one destination image beneath the trusted parent. The coordinator
closes the writer adapter before calling the no-replace publication mechanism,
then attempts staging cleanup on every outcome. Cleanup failures report the
retained staging basename and OS error alongside the primary outcome.
Unsupported safe publication retains the completed private image and reports
its location for caller recovery.
Existing destination objects are never
opened or truncated. Published/uncertain outcomes retain their distinct error
states. The library performs no process sandbox transition.
