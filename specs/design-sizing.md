# Automatic sizing and minimization

## Two policies

Automatic sizing, selected by `volume_bytes == 0`, chooses the volume's size
and the geometry settings the caller left open. It has two policies:

* **Default.** Candidates are tried in a fixed order and the first legal one
  wins. For FAT and exFAT the order is increasing allocation-unit size, so the
  default favors small units, which minimize per-file slack and match what
  native formatters choose for a volume of that size. For QNX6 the block size
  is a setting with a default of 1024 and the inode capacity follows the
  native ladder; for HFS Plus it is a setting with a default of 4096. The volume is the smallest legal one for the chosen
  candidate.
* **Minimize.** `FSIMG_WRITE_MINIMIZE`, CLI `-M`, chooses the candidate whose
  volume is the smallest in bytes, over every setting the caller left open.
  It is for images whose size matters more than headroom or native
  conventions: firmware payloads, test fixtures, archived trees.

Both policies are deterministic functions of the catalog totals and the
options, satisfy the requested free reserve, respect every explicit setting,
and never change the requested format or variant. Neither claims to optimize
over geometries the driver does not support.

## Settings as constraints

An explicit setting fixes its dimension in both policies. Minimization then
searches only the remaining ones; when none remain it is the default policy,
not an error. `volume_bytes` fixes everything the search would choose, so
`FSIMG_WRITE_MINIMIZE` with an exact size is `FSIMG_EINVAL`, reported by the
CLI as conflicting requested settings, like `-e` without `-R`. A fixed disk
(`gpt:disk`, `mbr:disk`) gives the volume its partition's length as an exact
size, so minimization with one is `FSIMG_EINVAL` as well. `min_free_bytes`
and `reserve_percent` are requirements, not dimensions: a minimized volume
still contains them. `root=` (`fat_root_entries`) fixes the FAT12/16 root
capacity, so a minimized root keeps the free entries it asks for; rounded up
to a whole sector, it is the same capacity under either policy.

## Dimensions per format

| Format | Searched under minimization | Kept from the default policy |
| --- | --- | --- |
| FAT12/16/32 | Allocation-unit size over the variant's legal units; root capacity on FAT12/16, the smallest sector multiple holding the root's slots unless `root=` fixes it | The variant, its cluster-count minimum and the 16-cluster type margin, the reserved-sector count, two FAT copies |
| exFAT | Allocation-unit size over the legal units | The 1 MiB minimum, one FAT, the recommended cluster-count rule as a tie-break only |
| QNX6 | Block size over the classic sizes; inode capacity, the catalog's inodes rounded to a whole table block; the block count at the alignment step instead of the 64-block search step | Byte order, the reserve percentage, the system-run recipe |
| QNX4 | Nothing: the block is 512 bytes and the block count is already the smallest that fits | The native default layout, the 128-slot `.inodes` minimum, the 4096-byte directory rounding |
| ImageFS | Nothing: the image is exactly its header, mountpoint, records, data and trailer | The 4-byte record and data alignment |
| HFS Plus and HFSX | Block size over the writer's sizes, 512 to 65536 bytes | The native B-tree node sizes, one extent per fork, the reserved head and tail blocks |
| APFS | Nothing: the block is 4096 bytes and the block count is already the smallest that fits | The native formatter's fixed regions for the block count, its minimum container and its sizing granularity |
| UDF | Nothing: the block is the sector, and the block count is already the smallest that fits | The 2048-byte default block, the fixed volume layout around the partition, the space bitmap's size |
| ISO 9660 | Nothing: the image is exactly its descriptors, path tables, directories, continuation region and data | The 2048-byte block and the writer's one layout |

The FAT type minimum is what makes the image the requested variant and is
not a dimension. The exFAT recommended cluster count is a guidance value, so
minimization may exceed it when a smaller volume results. The plan reports
the chosen unit's size and count and, for QNX6, the inode capacity, which
`c -n` prints on stdout and `c -vv` on stderr with its other details; `c -v`
names entries only. A minimized QNX6 volume has no spare
inodes unless `inodes=` is given, and the plan reports its free inode count,
since a native user of that volume cannot create files beyond it.

## Search

The entries pass accumulates, in one traversal of the layout rows, the
allocation total for every candidate allocation unit at once: one counter
per legal unit, at most seventeen, instead of one pass per candidate. Both
policies use that one pass. Each candidate's geometry is then computed as
the default policy already does, and minimization keeps the candidate with
the smallest `volume_bytes`.

Ties go to the candidate the default policy would have chosen, so the two
policies agree whenever the default is already minimal. Each format ranks
its candidates in a fixed preference order whose first legal member is the
default policy's choice, and a tie between other candidates follows the
same order:

* FAT: increasing allocation unit.
* exFAT: increasing allocation unit among the units whose cluster count is
  within the recommended count, the largest permitted unit included, then
  the remaining units in increasing order.
* QNX6: the default block size 1024, then the other open block sizes in
  increasing order.
* HFS Plus: the default block size 4096, then the other open block sizes in
  increasing order.

For QNX6 the block-count search first finds the smallest 64-block multiple
that fits, as the [driver design](design-qnx6-driver.md#planning) states,
then tests each smaller block count at the volume's alignment step within
that 64-block window and keeps the smallest that fits. The alignment step is
`4096 / B` blocks, so the volume stays a whole number of 4096-byte units and
an exact size of the minimized volume describes it again. The window never
goes below the 64 blocks that an exact size must hold. It holds at most 64
candidates, each a constant-time check against the accumulated totals, so
the exact minimum costs no further pass. A block size whose object or
system trees exceed the profile's depth limits is not a candidate.

For QNX4 the default block count is the smallest one that holds the fixed
layout, the objects and the bitmap, found by the short iteration the
[driver design](design-qnx4-driver.md#planning) states, so minimization
returns the default volume.

For HFS Plus the block count for one block size is the smallest that holds
the reserved blocks, the bitmap, the trees, the forks and the free reserve,
found by the raise-until-fit iteration the
[driver design](design-hfsplus-driver.md#planning) states. The writer's
B-tree node sizes are fixed, so the trees' node counts depend on the
catalog alone and are packed once; a candidate block size only rounds the
tree forks and the accumulated fork totals to whole blocks, so each costs a
few constant-time steps.
For APFS the default block count is the smallest one, in steps of the
native formatter's granularity, whose fixed regions (checkpoint areas,
internal pool and chunk structures, all functions of the block count),
metadata trees, file data and requested free space fit, found by the
binary search the [driver design](design-apfs-driver.md#planning) states,
so minimization returns the default container.

For UDF the default block count is the smallest whose partition holds the
space bitmap, the file set descriptor, the objects and the free reserve,
found by the short iteration the
[driver design](design-udf-driver.md#planning) states, so minimization
returns the default volume. UDF's logical block is its sector, and the
sector size is a property of the medium the volume is for, which no format's
minimization searches; FAT and exFAT search only the allocation unit above
it. The block size therefore stays the default or the caller's choice.

An ImageFS has no free space and no geometry: its size is derived exactly
from the catalog and the settings by the
[driver design's](design-ifs-driver.md#planning) layout pass, and the
plan reports it. Neither policy searches anything, so minimization returns
the same image, and a requested `volume_bytes` or `min_free_bytes` is
refused with `FSIMG_EINVAL`, which the CLI reports for `-s` and
`--free-space` as settings the format cannot honor.

An ISO 9660 volume has no free space either: its exact size follows from
the [driver design's](design-iso9660-driver.md#planning) layout pass, and
minimization returns the same image. Unlike ImageFS it accepts a
`volume_bytes` at least that size and a multiple of 2048, padding with
zero blocks the volume counts, since a padded volume is legal and some
media want it; `min_free_bytes` is refused with `FSIMG_EINVAL`.

Planning work grows by the number of candidates evaluated, never by a
second traversal of the catalog: each candidate geometry evaluated charges
one work unit, under either policy. The plan's reported scratch and work
figures include the search. The search keeps a fixed number of counters and
no per-candidate rows, tables or catalogs.

## Reporting and tests

`-n` and the plan information show the selected unit, block count and inode
capacity under either policy. `fsimg_plan_info_t` carries the unit count as
`allocation_units`, the clusters or blocks of the volume as an opened volume
reports them, and, for a format whose inode table bounds the files a native
user can create (QNX6), `inode_capacity` and `free_inodes`; both are zero for
the other formats. `root_entries` is the fixed root directory's capacity of
FAT12/16, which minimization shrinks to the root's slots unless `root=` fixes
it, and zero for the
formats whose root grows as a cluster chain or has no fixed size. Tests pin one catalog per format where the
default and minimized choices differ: a few large files, where a larger
allocation unit's smaller table outweighs its slack, must produce a smaller
volume under `-M` and an identical one under the default; a wide tree of
tiny files must choose the smallest unit under both. Every minimized image
passes the format's independent validators and reads back identically; byte
identity across source order, budget and scratch placement holds as for the
default policy.
