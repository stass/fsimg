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
CLI as conflicting requested settings, like `-e` without `-R`. `min_free_bytes`
and `reserve_percent` are requirements, not dimensions: a minimized volume
still contains them.

## Dimensions per format

| Format | Searched under minimization | Kept from the default policy |
| --- | --- | --- |
| FAT12/16/32 | Allocation-unit size over the variant's legal units; root capacity on FAT12/16, the smallest sector multiple holding the root's slots | The variant, its cluster-count minimum and the 16-cluster type margin, the reserved-sector count, two FAT copies |
| exFAT | Allocation-unit size over the legal units | The 1 MiB minimum, one FAT, the recommended cluster-count rule as a tie-break only |
| QNX6 | Block size over the classic sizes; inode capacity, the catalog's inodes rounded to a whole table block; the block count at the alignment step instead of the 64-block search step | Byte order, the reserve percentage, the system-run recipe |

The FAT type minimum is what makes the image the requested variant and is
not a dimension. The exFAT recommended cluster count is a guidance value, so
minimization may exceed it when a smaller volume results; `-v` names the
chosen unit and the plan reports it. A minimized QNX6 volume has no spare
inodes unless `inodes=` is given, and the plan reports its free inode count,
since a native user of that volume cannot create files beyond it.

## Search

The entries pass accumulates, in one traversal of the layout rows, the
allocation total for every candidate allocation unit at once: one counter
per legal unit, at most seventeen, instead of one pass per candidate. Each
candidate's geometry is then computed as the default policy already does,
and minimization keeps the candidate with the smallest `volume_bytes`. Ties
go to the candidate the default policy would have chosen, so the two
policies agree whenever the default is already minimal.

For QNX6 the block-count search first finds the smallest 64-block multiple
that fits, as the [driver design](design-qnx6-driver.md#planning) states,
then tests each smaller block count at the volume's alignment step within
that 64-block window and keeps the smallest that fits. The window holds at
most 64 candidates, each a constant-time check against the accumulated
totals, so the exact minimum costs no further pass.

Planning work grows by the number of candidates evaluated, never by a
second traversal of the catalog. The plan's reported scratch and work
figures include the search.

## Reporting and tests

`-n` and the plan information show the selected unit, block count and inode
capacity under either policy. Tests pin one catalog per format where the
default and minimized choices differ: a few large files, where a larger
allocation unit's smaller table outweighs its slack, must produce a smaller
volume under `-M` and an identical one under the default; a wide tree of
tiny files must choose the smallest unit under both. Every minimized image
passes the format's independent validators and reads back identically; byte
identity across source order, budget and scratch placement holds as for the
default policy.
