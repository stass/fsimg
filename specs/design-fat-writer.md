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

An ASCII name that fits 8.3 with its base and its extension each in a single
case uses its natural short name, with the lowercase-base (0x08) and
lowercase-extension (0x10) flags in directory byte 12 as Windows writes them,
so firmware that reads only 8.3 names finds `start4.elf` as `START4.ELF`.
Other names use long-name records and an ASCII alias `Fhhhhhhh`, with seven
uppercase hexadecimal digits. Each directory assigns increasing numbers in
catalog child order.
Every effective name in this alias family reserves its number, including names
that need a long-name record. A sorted reservation stream is merged with child
order; the next number skips reserved values. Thus aliases cannot collide with
any effective name or other alias. Assignment needs no repeated directory
search and is independent of memory budget and insertion order. The finite
directory slot limit bounds the number of reservations and assigned aliases.

Directory sizes include long-name slots, short entries, a zero terminator,
the root volume label, and non-root `.` / `..` entries. Directories cannot
exceed 2 MiB. FAT12/16 roots use a sector-aligned fixed region, selected to fit
the entries (at least 512 slots, exactly the rounded slots under
[minimization](design-sizing.md)) unless `root=` (`fat_root_entries`, the
`FSIMG_SETTING_ROOT_ENTRIES` setting only FAT12/16 records carry) fixes it.
Validation rounds a requested capacity up to a whole sector of 32-byte
entries and refuses one above the 16-bit count of 65535 with
`FSIMG_EINVAL`; planning refuses a capacity below the root's slots, the
terminator included, with `FSIMG_ERANGE`. The plan reports the capacity as
`root_entries`. FAT32 roots and
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
There is no convergence loop or image-wide search per entry. Allocation is
one next-fit pass in catalog ID order, starting with the FAT32 root: a run
takes the next free cluster, skipping past any [placed run](#fixed-placement)
it would overlap. Without placements nothing is skipped and the runs follow
each other in ID order. Empty files have no cluster.

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

## Fixed placement

The writer carries `FSIMG_SETTING_PLACEMENT` and honors the
[placement contract](design-placement.md) with one construction: **the data
area begins at the lowest placed offset.** The reserved area is padded with
zero sectors so that cluster 2 starts exactly there, and the lowest placed
file is cluster 2. Every other placement is a run at a fixed cluster inside
the data area. Nothing else in the layout changes meaning: the boot
sectors, FSInfo and backup sectors keep their positions inside the reserved
area, the FATs and the FAT12/16 root follow it, and the BPB reserved count
is the one field that grows.

Validation checks every offset against the sector size, and against the
cluster size when `cluster=` fixes it: an offset that is not a multiple is
`FSIMG_EINVAL` naming `place`. In the search, a cluster size that does not
divide every offset is not a candidate, which never empties the search,
since the sector-sized cluster divides every offset validation accepted. A
placed run then begins at cluster `2 + (offset - lowest) / cluster_bytes`
and holds `ceil(size / cluster_bytes)` clusters.

The data start fixes the rest of the geometry without a search. The
reserved count is the data start in sectors less two FATs and the root, and
must be at least the writer's normal count (1 for FAT12/16, 32 for FAT32)
and at most the 16-bit field's 65535. With automatic sizing the cluster
count is the clusters the allocation pass uses, gaps included, at least the
end of the highest placed run, plus the free reserve, raised to the
variant's minimum; the FAT length follows from that count and the volume
ends after the last cluster. With an exact size the cluster count is the
data area's length in clusters, which must reach the highest run's end and
the allocation's end, and the FAT length follows from it; no binary search
is needed because the data start no longer depends on the FAT length. The
type margins and cluster-range rules are unchanged.

Two consequences follow and are stated for users. The lowest placement
cannot lie deep inside a data area whose earlier clusters hold other files:
earlier bytes are reserved padding, zero, belonging to no file, which a host
may use for raw sectors a FAT driver never touches. And since the padding is
reserved sectors, the lowest offset is capped by the reserved-count field:
about 32 MiB with 512-byte sectors (65535 sectors plus the FATs and root),
proportionally more with larger sectors. Higher placements may sit anywhere
in the data area.

The allocation pass is the next-fit pass of the previous section with the
placed runs fixed first, sorted by cluster. An unplaced run that would
overlap the next placed run starts after it; the gap left before the run
stays free. Placed runs are checked in sorted order not to overlap each
other for the candidate's cluster size. The pass is one traversal in ID
order, deterministic and independent of the memory budget, and by
construction no unplaced run, directory or FAT32 root lies inside a placed
run. Sizing runs the same pass as a simulation: the candidate counters of
the one catalog traversal hold, per cluster size, the next free cluster and
the index of the next placed run instead of a plain total, so the pass
still visits each row once for all candidates. The FAT32 root takes the
first free cluster the pass gives it, which the BPB's root cluster field
records; `..` encodes zero when the parent is the root entry, whichever
cluster the root holds.

Emission streams the FAT in cluster order through the highest used cluster,
merging two sorted sequences: the unplaced rows, whose clusters increase
with catalog ID, and the placed runs by cluster. A cluster in neither is
free and encodes zero, as the zero-initialized tail of the table does. The
FSInfo free count is the clusters not allocated, gaps included, and its
next-free hint is the cluster after the highest used run. Payload copies are
unchanged: each file is copied into the one run its row names.

Rejections follow the diagnostic rules above with two more candidate
reasons: the lowest placement leaves no room before the data area for the
reserved sectors, FATs and root, and two placed runs overlap. Each names the
offsets involved; a reserved count above 65535 is the existing field-limit
failure. Mixed reasons across candidates report conflicting settings as
they do today.

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
