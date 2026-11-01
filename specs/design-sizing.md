# Automatic sizing and minimization

## Two policies

Automatic sizing, selected by `volume_bytes == 0`, chooses the volume's size
and the geometry settings the caller left open. It has two policies:

* **Default.** Candidates are tried in a fixed order and the first legal one
  wins. For FAT and exFAT the order is increasing allocation-unit size, so the
  default favors small units, which minimize per-file slack and match what
  native formatters choose for a volume of that size. For QNX6 the block size
  is a setting with a default of 1024 and the inode capacity follows the
  native ladder. The volume is the smallest legal one for the chosen
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
still contains them.

## Dimensions per format

| Format | Searched under minimization | Kept from the default policy |
| --- | --- | --- |
| FAT12/16/32 | Allocation-unit size over the variant's legal units; root capacity on FAT12/16, the smallest sector multiple holding the root's slots | The variant, its cluster-count minimum and the 16-cluster type margin, the reserved-sector count, two FAT copies |
| exFAT | Allocation-unit size over the legal units | The 1 MiB minimum, one FAT, the recommended cluster-count rule as a tie-break only |
| QNX6 | Block size over the classic sizes; inode capacity, the catalog's inodes rounded to a whole table block; the block count at the alignment step instead of the 64-block search step | Byte order, the reserve percentage, the system-run recipe |
| QNX4 | Nothing: the block is 512 bytes and the block count is already the smallest that fits | The native default layout, the 128-slot `.inodes` minimum, the 4096-byte directory rounding |
| ImageFS | Nothing: the image is exactly its header, mountpoint, records, data and trailer | The 4-byte record and data alignment |

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

An ImageFS has no free space and no geometry: its size is derived exactly
from the catalog and the settings by the
[driver design's](design-ifs-driver.md#planning) layout pass, and the
plan reports it. Neither policy searches anything, so minimization returns
the same image, and a requested `volume_bytes` or `min_free_bytes` is
refused with `FSIMG_EINVAL`, which the CLI reports for `-s` and
`--free-space` as settings the format cannot honor.

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
the other formats. Tests pin one catalog per format where the
default and minimized choices differ: a few large files, where a larger
allocation unit's smaller table outweighs its slack, must produce a smaller
volume under `-M` and an identical one under the default; a wide tree of
tiny files must choose the smallest unit under both. Every minimized image
passes the format's independent validators and reads back identically; byte
identity across source order, budget and scratch placement holds as for the
default policy.
