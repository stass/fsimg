<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com> -->

# Best-effort salvage extraction

## Contract

`--salvage` attempts whole-volume extraction from an openable damaged image.
It makes **no guarantees about completeness or trustworthiness of its output**.
It neither repairs the image nor verifies the recovered tree. Ordinary opening
checks, verification, list and info retain their contracts. Salvage accepts no
member selection, stdout extraction or mtree output.

## Files

For each regular file, salvage copies the logical prefix before the first
noticed damage. A damaged `foo` becomes `foo.partial`, shorter than its declared
length, with no zero filling and no recovery of intact later ranges. A file
with no recoverable bytes is reported and skipped; no empty `.partial` is
created. An undamaged empty file is extracted normally. Filesystem-defined
zeros keep their ordinary meaning and never stand in for damaged content.

The extractor completes the format's allocation/index pass before creating the
file, including indexing that ordinary reading performs lazily. The pass uses
the existing scratch-backed extent index and its stopped mark
(`lib/meta/extent.c`). It preserves the prefix at image damage and determines
the first unavailable logical byte from the indexed mappings and the readable
slice. Whole indexed units can extend past the slice; the stopped mark alone
is not a byte cutoff. ISO 9660's content domain admits only readable whole
blocks. No mapping outside the content domain supplies recovered bytes.

The operation is common: `fsimg_reader_salvage_plan` resolves the file's extents
from its first byte and stops at a lookup the driver rejects or at the end of
the domain or slice. A format supplies its resolve and a replay that keeps the
allocation before the damage instead of rejecting the entry. A planned file is
shortened in the current entry, so its charge, output size and reads see only
the prefix, and a garbage size field neither spends the budget nor ends the walk.
A prefix past the output budget cannot be charged whatever follows it, so the
walk stops there with the charge's limit error, and a clean file over the budget
is terminal.

An extent a driver holds decoded instead of mapping reads no image, so neither
the domain nor the slice clips it: every byte of it is available, and damage to
what it was decoded from is a rejected lookup. Planning such a file decodes it, up
to the extent that crosses the output budget.

A linked cluster allocation keeps each cluster once. A cluster belongs to the
prefix when its own link reads and validates, its allocation bit included, and
the chain ends at its first repeat, found after the walk proves a cycle and
before any repeat is delivered. A chain broken before the file's size ends the
file there even past the initialized length. Past the slice the initialized
length decides, since defined zeros need no storage.

Damage past the file's size, in any format's allocation, is not damage to the
file: every byte is available, so the file keeps its name and no loss is reported.

The prefix length and damage decision select the name at exclusive creation.
There is no rename and no added sandbox authority. Copying uses only the
planned prefix through ordinary content delivery. Damage encountered during
copying is terminal under the existing failure contract.

If a file's output name is taken, `foo.partial` or a clean name alike, exclusive
creation reports the collision and salvage skips the file. It never overwrites
or tries a numbered fallback. A suffixed name over the name limit, or a suffixed
path over the path limit, is reported and skipped the same way. Name validation and the ordinary policy for unrepresentable
names still apply.

Each name is handled independently, including several names for one object.
An earlier name supplies no proof about a later name's content. There is no
first-name-only recovery policy or retained proof of an object's cleanliness.

## Directories and reader state

A bad directory is reported and its child listing is skipped; extraction
continues with its siblings. If the root listing is bad, the walk ends with
that loss. The report does not invent a count of inaccessible descendants.

The recovery point is the walker's descent into a child: its parent's prepared
listing and cursor already identify the next sibling. On a directory decode
or namespace-validation failure, preparation releases its temporary state and
rewinds its appended records to the existing stream mark. A failed rewind is
terminal. The walker keeps the parent level, clears pending descent and resumes
there. Claims and resource charges already made remain in effect.

Recovery happens inside the walker before failure poisons its stack or reaches
`fsimg_reader_abort`, which frees the walk and closes driver state. The reader
remains iterating after a handled loss; terminal failures leave it failed.
`fsimg_reader_skip_directory` prunes a current directory before descent; it does
not revive a failed reader. A failed entry decode during replay consumes that
parent record, reports the entry loss and continues without descending.

## Failure boundary and limits

Classification follows the operation that fails, not a public error code:

* Image decoding or allocation validation during file planning stops that file
  at its first damage; directory decoding or namespace validation skips that
  listing, and entry decoding during replay skips that entry.
* Exclusive creation finding any name occupied, for a file or a directory, skips
  and reports that entry, and a directory's subtree with it. A clean `foo.partial`
  following a damaged `foo` costs only that name and its subtree.
* Every other failing operation is terminal: opening checks, resource admission,
  cancellation, host input I/O and stability checks, output creation/copying/
  finalization, scratch/index storage and reporting. Changed input, output
  failures and scratch failures never become recoverable image damage.

The error's operation identifies a decoder's failure: a driver's prepare or
replay rejecting the image reports `FSIMG_ECORRUPT` or `FSIMG_EUNSUPPORTED` under
`FSIMG_OP_READER`, and scratch storage reports `FSIMG_OP_METADATA` even for
corruption it detects, the extent index's checks of its records included. An
appended extent outside its domain is image damage and a lookup past a stopped
index is the image ending, so both report `FSIMG_OP_READER`. Everything else is
terminal, so a new failure is terminal until a decoder declares it. A scratch
record that scratch storage delivers intact and a driver's own decoding then
rejects cannot be told from image damage and is reported as a loss, like the
damage that decoding exists to notice.

An entry safe extraction cannot create, by its kind or its name, would end the
walk under the ordinary policy, so `--salvage` implies `-k`. Each one is reported
and counted as a skip or omission, as `-k` does, its children going with it. It
says nothing about damage, so it is not a loss, and a clean image holding a
symbolic link succeeds. The library option alone keeps the ordinary policy.

All existing limits and cumulative accounting apply. Salvage adds no work
ceiling or separate budget. A caller can set `max_work`; heavy damage can be
slow when it does not. Resource exhaustion terminates extraction.

Every extraction safety rule is unchanged: private root, descriptor-relative
exclusive creation, no-follow protections, validated names, budgets, safe
metadata policy and sandbox confinement. Bytes outside the content domain are
never delivered. In-domain bytes are not proof of ownership: a cross-linked
block can appear in a clean-named file. `.partial` marks only damage salvage
notices; its absence promises neither an intact file nor trustworthy bytes.

## Results and reporting

Any loss returns `FSIMG_EPARTIAL` and CLI exit 1, with the private tree retained
and identified as incomplete. A terminal failure takes precedence and retains
its ordinary error, including one found after the walk: a failure in cleanup,
such as flushing the destination's parent, or in the final report replaces the
partial result, whose total goes with it. A capped report carries bounded loss
examples and the total number of noticed losses, even when examples are omitted;
unknown descendants are not counted as known losses. Reports identify shortened
and skipped paths and their causes. The library does not print diagnostics. Completing the walk
without a noticed loss returns ordinary success, with no integrity guarantee.

The library reports through the context's loss callback, the rendered path with
one `FSIMG_LOSS_*` kind as its cause, for the first `FSIMG_LOSS_EXAMPLES` losses
of a reader. `salvage_losses` in the statistics and the extraction result holds
the total, and the callback's consumer keeps the examples. The entry callback
reports a shortened file under its output name.

A CLI worker reports a completed walk the same way whether or not it lost
anything: its completion frame carries the count, and the coordinator turns a
nonzero count into `FSIMG_EPARTIAL`, so a worker reporting losses cannot report a
clean result. Loss examples and metadata loss examples have separate allowances. A
count below the examples sent, a count on a job not asked to salvage, and an
error frame carrying `FSIMG_EPARTIAL` are protocol violations. A count of zero
remains the worker's word, like a
[verification verdict](design-security.md#coordinator-and-worker), since the coordinator has no view of
the volume.

## Tests and acceptance evidence

* Pair clean, empty and defined-zero files with strict extraction; compare bytes.
* For each format, damage a file before its first byte and midway; check exact
  shortened prefixes, `.partial` names, no zero fill and readable siblings.
* Damage child/root listings and replayed entries; check sibling recovery,
  reader state, unknown descendant counts and terminal rewind failure.
* Exercise occupied names, clean and `.partial`, for files and directories,
  including host-equivalent spellings, and repeated object names; check no
  overwrite, merge, fallback or alias proof.
* Check capped reports with exact totals, partial status, exit 1 and retained
  roots; inject terminal resource, I/O, changed-input, scratch and copy failures.
* Exercise domain boundaries and extraction breakout attempts under the ordinary
  sandbox; check rejected selection/stdout/mtree and unchanged verify/list/info.
