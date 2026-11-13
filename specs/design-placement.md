# Fixed file placement

## Purpose

A boot medium often wants one file at a known byte offset of its volume so
that firmware or a test harness can read it without a filesystem parser: a
kernel at 4 MiB of a FAT32 slot, a configuration file at 3 MiB of a FAT16
record volume. Placement is the write setting that asks for this. The caller
names image files and, for each, the byte offset from the volume start at
which its payload must begin as one unbroken run. The writer lays the volume
out around those runs or refuses.

Placement is one format-neutral setting with one capability bit,
`FSIMG_SETTING_PLACEMENT`, carried through the
[settings catalogue](design-settings.md), the API options, the command line,
the worker job, planning and verification like every other filesystem
setting. A format that supports it carries the bit in its write mask and
states in its own design how it honors the contract below; a format without
the bit refuses a non-empty list through the common applicability check. The
FAT12/16/32 writer carries the bit; its construction is in the
[FAT writer design](design-fat-writer.md#fixed-placement).

## The contract

A placement of file `P` at offset `O` guarantees, in the created volume:

* the bytes of `P` occupy image bytes `[O, O + size)` in order, as one
  contiguous run;
* no other file's payload and no metadata lies inside the allocation units
  that run occupies; a trailing partial unit belongs to `P` and holds zeros
  past its end.

The guarantee is a property of the created image. A later writer of the
volume, such as a guest filesystem driver that rewrites the file, may move
it; fsimg does not mark the run as pinned, because no format in scope has
such a mark. The README and the manual page state this for users.

Offsets are byte quantities from the start of the volume, not of the disk:
in a partitioned image the volume begins at its partition. Several files may
be placed in one volume. A placement may lie anywhere in the format's data
area, not only at its start; the format's design says which offsets it can
honor.

## Interface

```c
typedef struct fsimg_placement {
	const char	*path;		/* Absolute image path, copied */
	uint64_t	 offset;	/* Bytes from the volume start */
} fsimg_placement_t;

#define FSIMG_PLACEMENT_MAX 8u
```

`fsimg_write_options_t` holds `const fsimg_placement_t *placements` and
`uint32_t placement_count` beside the ISO 9660 boot paths, under
`FSIMG_SETTING_PLACEMENT`. `placement_count` is the one fact that says
whether the setting is in use: zero is the default and the pointer is then
not read; a nonzero count with a NULL pointer is `FSIMG_EINVAL`. Writer
construction copies the array and every path into writer-owned storage, as
it copies the label, mountpoint and boot paths, so the caller's array may be
released once `fsimg_writer_new` returns. A path names an entry of the image
being written, never a host file: the field carries no descriptor and grants
no authority.

In the catalogue the row is `place`, a filesystem write setting of a new
value kind, a borrowed placement array with its count, and a new syntax
class, *placement*: `PATH@OFFSET`, split at the last `@` so a path may
contain `@`, the path a text within the image-path capacity boot paths use
(1024 bytes and a terminator) and the offset a byte quantity in the command
line's unit syntax. The row declares the catalogue's one list multiplicity
with capacity `FSIMG_PLACEMENT_MAX`: the command line accepts
`-o place=PATH@OFFSET` once per placement, appends each occurrence, and
treats the occurrence past the capacity as a syntax error, exactly as it
treats a repeated single-valued key. The catalogue's default test is
`placement_count == 0`.

The command line for the two examples of the opening section:

```
fsimg c -f slot.img -F fat32 -s 512M -o place=/kernel8.img@4M -C slot .
fsimg c -f record.img -F fat16 -s 8M -o cluster=512 -o place=/autoboot.txt@3M -C record .
```

## Transport

The worker job record carries the list after the EROFS fields: an eight-byte
little-endian count, then `FSIMG_PLACEMENT_MAX` records of an eight-byte
offset and a path in the image-path field width boot paths use
(`FSIMG_JOB_BOOT_PATH_BYTES`), NUL-terminated and zero after the terminator.
Records past the count are all zero. The count is the presence: an empty
path is never a value here, since validation refuses it, so no presence bits
are needed. Decoding refuses a count above the capacity, an unterminated or
empty path within the count, and a nonzero byte in a record past the count,
as an invalid job.

The bound exists for two reasons, and both are what it prevents. The job
stays one fixed-length frame the decoder validates by length, with no
variable section whose count another field must be trusted to state; and
planning holds the placements as one small array it sorts in place and
scans per catalog entry, needing no scratch array and no per-entry search
structure. Eight covers a boot medium's fixed artifacts (kernel, device
tree, initial RAM disk, configuration) with margin, and costs the record
about 8 KiB. A larger list is a different feature, a layout description,
not a bigger setting.

## Common validation

Writer construction checks the list before the format's validation runs,
naming the `place` key in `error.path`:

| Condition | Status |
| --- | --- |
| `placement_count` above `FSIMG_PLACEMENT_MAX`, or nonzero with a NULL array | `FSIMG_EINVAL` |
| A NULL or empty path, a path that is not an absolute image path under the common path rules, or one past the image-path capacity, the context's path limit | `FSIMG_EINVAL`; a path deeper than the context's depth limit keeps the path rules' `FSIMG_ELIMIT` |
| Two placements with the same path, or two with the same offset | `FSIMG_EINVAL` |
| A nonzero count for a format whose write mask lacks `FSIMG_SETTING_PLACEMENT` | `FSIMG_EINVAL`, from the common applicability check |

The format's validation then checks what depends on its geometry settings,
such as alignment, with the offsets as given. Offsets are `uint64_t` and
every sum or difference of an offset and a size, and every division by an
allocation unit, uses checked arithmetic; an overflow is `FSIMG_ERANGE`.

## Resolution during planning

Planning matches each placement path against the frozen catalog by exact
comparison with the entry's own path, as the ISO 9660 writer matches its
[boot paths](design-iso9660-driver.md#writer-settings): the catalog's paths
are normalized at admission, so the setting must spell the path as the
catalog does, and no resolution, prefix match or case folding takes place.
The refusals and their statuses are the boot paths' own:

| Condition | Status |
| --- | --- |
| No entry has the path | `FSIMG_EINVAL`, "place names no file in the image" |
| The entry is not a regular file, or is empty and so has no allocation | `FSIMG_EINVAL`, "place must name a nonempty regular file" |
| The format cannot lay the volume out around the placed runs: an offset it cannot reach, two runs that collide, or a volume too small | `FSIMG_ERANGE`, with the format's own diagnostic |

Matching costs one comparison per placement per catalog entry, bounded by
`FSIMG_PLACEMENT_MAX`, inside the pass the format already makes over its
rows; no second traversal and no name index.

## Per-format obligations

A format that carries the bit states, in its design:

* which offsets it can honor: its alignment rule, the lowest and highest
  offsets its on-disk fields allow, and what the volume looks like around a
  placed run;
* how its allocation keeps every other allocation out of the placed runs by
  construction, not by a check after the fact;
* how automatic sizing and [minimization](design-sizing.md) account for the
  placed runs and the gaps they leave;
* its refusal diagnostics, under the format's existing diagnostic rules.

The reader and verifier of the format need no knowledge of placement: a
placed volume is an ordinary legal volume whose allocation happens to have
gaps, and the format's full verification accepts it as it accepts any volume
with free runs between files.

## Reporting and verification

The plan reports nothing new. A placement's result is its own definition:
the file begins at the offset asked for, so `-n` and `fsimg_plan_info_t`
gain no field, and a consumer who needs the offset has it already. The
plan's allocation and free totals count the placed runs as allocated and
the gaps between runs as free, which is what the volume's own structures
say.

The staged verification of `cW` is the ordinary read-side verification and
asserts nothing about placement. It would have to learn the settings the
image was created with, a channel no other setting needs, to confirm a fact
the tests below establish once and the creation path cannot lose: the
emitter copies each payload to the single run the plan assigned it, and the
plan assigned the run from the offset.

## Security

Placement paths are image paths matched against the writer's own catalog.
They open nothing, name no host object and are never rendered into a host
path. Offsets come from the caller and are untrusted arithmetic inputs:
every derived quantity is checked, and no offset indexes a buffer before
planning has proven it lies within the planned volume. Every byte of the
padding a placement creates, and every allocation unit left free between
runs, is zero from the common emission's sink initialization; the writer
never emits uninitialized bytes to reach an offset.

## Tests

Each case names what passes and the seeded failure that proves the checked
code ran.

* **Round trip, one placement.** A FAT16 and a FAT32 volume created with
  `place=` read back, enumerate and extract identically to the source, and
  the bytes at the offset equal the placed file's bytes for its whole
  length. Seeded failure: the same catalog created without the setting
  stores the file elsewhere, and the byte comparison at the offset fails.
* **Several placements with gaps.** Three placed files at increasing
  offsets with unplaced files and directories between them in catalog
  order. Decoding the FAT with the specification harness's codecs shows
  each placed file's chain starting at the cluster its offset names and
  running contiguously, and no other chain or directory intersecting a
  placed run. Seeded failure: an image copy with one entry of a placed run
  redirected is rejected by the same check.
* **The requester's volumes.** The 512 MiB FAT32 slot with the kernel at
  4 MiB and the 8 MiB FAT16 record volume with 512-byte clusters and the
  file at 3 MiB both plan, create and verify; the record volume's sector
  2048 lies in the reserved area and holds zeros. Seeded failure: the same
  record volume with a placement at 3 MiB plus one sector is refused as
  misaligned.
* **Every refusal.** One case per row of the two tables above and per
  format refusal, each paired with the same options corrected so the
  creation succeeds: count past the bound, NULL array with a nonzero count,
  relative path, duplicate path, duplicate offset, unsupported format
  (exFAT, ISO 9660), missing file, directory, empty file, misaligned
  offset, two colliding runs, an offset with no room for the metadata
  before it, an offset past the reserved-count field, an exact size too
  small for the highest run.
* **Independent tools.** `fsck.fat -n` reports the placed volumes clean,
  `mdir` lists the placed files, and `mtype` returns their bytes. Seeded
  failure: a copy whose FAT entry for a placed run is zeroed is reported by
  `fsck.fat`, so the oracle is known to inspect the run.
* **Reproducibility.** Two creations of a placed volume are byte-identical,
  as are creations from differently ordered sources and under different
  memory budgets; the padding before the FATs and every free unit between
  runs is zero. Seeded failure: the stale-data scan is run against an
  image copy with one nonzero byte in the padding and reports it.
* **Minimization.** `-M` with a placement keeps the data start at the
  lowest offset and still chooses the smallest-volume cluster size among
  the sizes that divide every offset; an offset that only the sector-sized
  cluster divides pins the search to it.
* **Transport.** A job with `FSIMG_PLACEMENT_MAX` placements encodes and
  decodes to equal values; a count of nine, an unterminated path within the
  count and a nonzero byte past the count are each an invalid job.
* **Command line.** `place=` repeats up to the bound; the next occurrence,
  a value without `@`, an empty path, an empty or malformed offset and an
  offset with trailing junk are syntax errors (status 2); a path containing
  `@` splits at its last `@`; a placement for a format without the bit is
  refused before the worker starts when `-F` names the format.
