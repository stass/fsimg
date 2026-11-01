# Prepared directories and common traversal

Every reader exposes a volume as a tree of directories whose children are
listed in on-disk order and whose names resolve unambiguously. This document
defines one contract for that: a driver prepares a directory, and a common
walker owns everything that happens after preparation succeeds. Drivers learn
nothing about stacks, paths, pruning, or selection; the walker learns nothing
about directory encodings, allocation, or name folding.

The [architecture](architecture.md#writer-and-reader-independence) assigns the
owners. [Storage](design-storage.md#reading-and-traversal) states the bounds.
Driver designs ([FAT](design-fat-reader.md), [exFAT](design-exfat.md),
[QNX6](design-qnx6.md), [QNX4](design-qnx4.md),
[ImageFS](design-ifs-driver.md), [HFS Plus](design-hfsplus-driver.md))
define only their decoding, keys, and locators.
Host source scanning and safe extraction have separate authority and lifetime
rules and do not use this walker.

## Owners

| Owner | Owns | Does not own |
| --- | --- | --- |
| Driver | Decoding directory storage, entry framing and metadata, child allocation and parent references, directory-cycle claims, comparison keys, private locators | Traversal order, ancestor state, paths, depth/path/entry limits, selection |
| Preparation | Admitting entries: charging them, enforcing depth and path limits, spooling records, validating the namespace, publishing the prepared directory | Interpreting record payloads |
| Walker | Depth-first order, deferred descent, pruning, path assembly, ancestor cursors, directory-stream lifetime, the current-entry snapshot | Name comparison or any format field |
| Selector | Resolving an image path one component at a time and choosing the traversal scope | Directory decoding or private namespace indexes |

Preparation, walker, and selector are common reader code in `lib/read/`.
Structural verification iterates through the same walker; its ownership
claims are emitted by the driver while preparing directories and resolving
file allocation. It has no second traversal engine.

## Prepared directory

A prepared directory satisfies one invariant:

> Its children have valid framing and metadata, its lookup namespace is
> unambiguous, and its entries can be replayed in original order.

It consists of a run of entry records, in on-disk order, on the walker's
spillable directory stack. Each record holds its class and its component,
validated, NUL-terminated UTF-8 for a delivered entry and the raw bytes for
an [omitted](#omitted-names) one, which only the rendering function reads,
followed by a bounded, explicitly encoded driver payload. The payload
carries what the driver needs to rebuild the entry snapshot and its private
locator: for example, the FAT 32-byte short entry and the
cluster its `..` record must name, or the exFAT file and stream extension
entries. Records never contain native structures, pointers, or padding.

An incomplete directory is private to its preparation. The walker receives a
directory only after namespace validation succeeds, and owns its
lifetime from then on. No caller can observe a child whose siblings have not
been validated, so namespace validation is a property of every exposed
directory, not a step each traversal path must remember.

Preparation does not make scratch bytes permanently trustworthy. Replay
re-checks record framing and lengths, and the driver re-validates the decoded
payload fields and locator before they become the current entry.

## Namespace

The shared invariant is:

> Multiple spellings may identify one directory entry. One comparison key
> cannot identify two different directory entries.

While decoding, the driver emits one or more comparison keys for each admitted
entry. FAT emits its long-name key and alias key; exFAT emits the name mapped
through the volume's validated upcase table; QNX6 emits the case-sensitive UTF-8
bytes; HFS Plus emits the stored name decomposed and, on a case-insensitive
volume, folded through its pinned table. Keys are opaque, bounded byte strings to common code. Preparation stores
key records `(entry position, key)` in a second stream, sorts them externally,
and rejects equal keys with different entry positions. Duplicate keys for the
same entry are permitted. The key stream is released when validation completes.
The driver derives an entry's keys with one function from its entry record,
the displayed name and payload. Preparation and selection both call it,
selection on the replayed and re-validated record.

Entry identity is the position of the entry's record within the prepared
directory, a directory-entry occurrence. It is never inode or allocation identity: separate
directory entries can legitimately reference one object, and that never permits
two names to share a key within a directory. Allocation-owner identity belongs
to [ownership verification](design-storage.md#verification-scope).

The driver supplies the error classification and diagnostic text for an
ambiguous namespace. Entry names travel only in path fields, never in
message text: common code names the second of the colliding entries through
the error's rendered path and describes the first by its position in the
directory in the message, never by raw key bytes.

## Selection

A query component matches an entry when the driver's key for the query equals
one of that entry's namespace keys. The driver produces both keys; the selector
compares bytes. Selection therefore uses exactly the keys that namespace
validation proved unambiguous, so a lookup cannot reach an entry the
validation did not examine and cannot pick the first of two equivalent names.
A component that the driver cannot represent is an invalid query, not a miss.

Resolved paths retain displayed names. Off-branch subdirectories are pruned
after their parent is prepared, without being opened. Selecting one child still
requires the parent's complete namespace validation.

Several members resolve together in one walk. A member keeps only how many of
its components have resolved. The walk is depth first, so while it stays
below a member's resolved prefix every entry it returns begins with that
prefix, and the first entry at the prefix's depth or above ends the member:
it can resolve no further and select nothing more. An entry's relation is
the strongest any member gives it: inside a resolved member's subtree, then
a member's target, then an ancestor of an unresolved member. A directory is
pruned unless some member needs its children, and its omitted children are
counted only when some member selects them. The walk returns each entry
once, so an entry that several members select, through nesting, repetition
or aliases, is processed once without any record of what was returned, and
no member costs more than its spelling and a few counters. A member that
ends unresolved matched nothing, and the walk fails naming it; otherwise the
walk ends once no member can select anything more.

## Traversal

The walker performs iterative depth-first traversal. Its ancestor stack is
bounded by the configured depth limit, and each level retains only the start of
its record run, its replay cursor, and its path length. One charged path buffer
holds every ancestor prefix. A directory entry is returned before its children.
Descent is deferred to the next advance, so the caller can prune the subtree
first.

Every retained directory lives in one stack stream. A child is prepared above
its parent's unread records, so only the deepest directory is replayed and its
run always ends the stream. An exhausted directory is released by rewinding the
stream to the start of its run, and later preparation reuses that storage.
Traversal therefore holds one scratch store, and at most one scratch descriptor,
however deep it goes: a legal tree within the depth limit cannot exhaust the
descriptor limit. The stack is released when the root is exhausted.

The driver opens a child directory from the current entry's locator, and claims
its storage there. For FAT and exFAT that claim uses a paged exact bitset, so
re-entering claimed storage is corruption whether it comes from a cycle or a
shared chain. A driver with different object identity supplies an equally
bounded exact scheme.

## Charging and limits

Preparation charges each entry against the session when it admits the entry,
and enforces the depth and complete-path limits at the same point. Replay does
not charge the same entry again, and it cannot exceed a limit that admission
already enforced. Decoding, cache misses, and sorting consume work units in the
ordinary way. The first advance into a wide directory can therefore do work
proportional to its width, but opening the volume stays bounded and no
whole-volume tree is materialized.

## Omitted names

A driver admits a child once its name has passed the driver's native checks,
the ones whose failure is corruption, such as an embedded NUL or an empty
name. Preparation then assigns the child one of two classes. A format whose
reader operations, which its descriptor names, declare the QNX-family name
profile (QNX4, QNX6 and ImageFS) has every child's component checked by the
one common profile function:
strict UTF-8, no NUL, slash, control byte or `0x7f`, neither `.` nor `..`.
A component under the profile makes the child *delivered*; a natively legal
component outside it makes the child *omitted*, under the
[architecture's rule](architecture.md#names-and-metadata). A format
that declares no profile, FAT, exFAT and HFS Plus, admits every child as delivered,
since its driver derives validated names by its own rules. Common code
branches on the declared property, never on a format name, and no driver
chooses a class. The same function validates a query component in every
QNX driver's `key`, and the ImageFS child pass calls it to count omitted
nodes at open, so there is one rule for what the profile accepts.

The class is the only difference between the two kinds of child. An
omitted child is decoded, validated and charged like a delivered one, its
record is spooled with its class, and its keys enter the namespace, so a
duplicate of an omitted name is still a duplicate; the raw bytes are
compared only as keys, for duplicate detection, and a query never matches
them because a query component outside the profile is `FSIMG_EINVAL`
before any directory is consulted.

An omitted record carries no displayed UTF-8 name. The walker's path
buffer, every path limit and every key work on raw bytes. Every path the
library emits is rendered, with one exception: `fsimg_entry_t.path`, an API
value that only a delivered entry ever has. The single rendering funnel is
`fsimg_path_render()`, which renders a path into a caller-supplied buffer
and returns whether it truncated; its input is either a walker path with
its component classes or a plain path from the catalog, the source scan or
the extractor, treated as all delivered. It has three callers.
`fsimg_error_path()` renders into `fsimg_error_t.path` and stores the
result in `path_truncated`. The loss-report callback's path is rendered
into a buffer sized for the worst case of its input, four bytes per input
byte up to the complete-path limit `max_path_bytes` plus the terminator,
`4 * max_path_bytes + 1`, so it is never truncated. The walker renders the
paths of its own errors, such as a namespace collision or an extraction
refused for an omitted entry; a verification finding is one of these and
reaches the caller as an `fsimg_error_t`, so its path has the error record's
bound, truncation rule and flag. For library callers this is a contract: a
callback or finding path is rendered, so it holds `\\` where
`fsimg_entry_t.path` holds `\`. The rendering is an escaping:
in every component, delivered or omitted, a backslash is written `\\`; in
an omitted component every byte outside `0x20` to `0x7e` is written `\xNN`
with lowercase hex; a delivered component is otherwise unchanged. It is
injective because the only backslashes in the output are the ones it
writes, and `\\` and `\xNN` parse apart left to right, so unescaping
recovers the raw bytes exactly. Truncation into the bounded error path
keeps the tail, which names the entry, cuts only on a whole rendered unit,
never inside `\\`, `\xNN` or a UTF-8 sequence, and sets the error's
`path_truncated` flag.

The walker owns what omission means. Every admitted child becomes the
walker's current entry in traversal order, marked by its class and by
whether an ancestor is omitted, which the ancestor cursors show; delivery
to callers filters on that mark:

* `next()` returns only a delivered entry with delivered ancestors, so a
  listing never shows an omitted entry or anything below it. Selection
  resolves only through delivered entries, and no query can spell an
  omitted name.
* The walker reports a marked omitted entry to the extractor through an
  internal interface, so `fsimg_extract_at()` is the only consumer that
  acts on it; a library caller copying through `next()` gets listing
  behaviour, no failure and `omitted_entries` counted. Extraction of a
  scope that contains an omitted entry fails with `FSIMG_EUNSUPPORTED`
  naming the entry, rendered, unless the caller selects
  `FSIMG_EXTRACT_SKIP_UNSUPPORTED`, which omits it instead, exactly as for
  an entry of an unsupported kind. A partial output is never success, so
  the omission is an explicit choice.
* The walker counts each omitted subtree root it passes within the
  traversal scope, an omitted entry whose ancestors are all delivered,
  listed by a directory whose children the operation selects, in
  the operation's result (`fsimg_extract_result_t` for extraction,
  `fsimg_verify_report_t` for verification) as `omitted_entries` and in the
  context
  statistics' field of the same name; the result field is the count an
  operation reports, the statistic accumulates over the context. A child
  below an omitted directory is never counted. The walker is the only
  owner of `omitted_entries`; the ImageFS `omitted` info detail precomputes
  the same number with the same function. A listing, an extraction under
  the skip option and a full verification of one scope report the same
  number, absent pruning by a selection, and `skipped_entries` keeps its
  one meaning, entries of unsupported kinds the extractor skipped on
  request.
* An operation that delivers entries never descends into an omitted
  directory: preparation's cost stays the directory's children, and an
  omitted subtree costs its root's record and nothing more.
* Full verification descends into an omitted directory and replays every
  omitted entry exactly as it does a delivered one, so the driver's claims,
  ownership runs, reference records and index builds cover the omitted
  subtree and the reconciliation that follows misses nothing; payload
  read-through reads omitted files and links too. The report's files,
  directories and logical bytes count delivered entries; bytes read and
  allocated units cover everything walked; the omitted subtree roots are in
  `omitted_entries`; and the report is complete, since every omitted
  object was checked. Corruption below an omitted name is corruption,
  found by the same checks as anywhere else.

## Snapshots and borrowing

The current entry snapshot, its path, and the driver's current locator are
borrowed. They are valid until the next advance begins, a terminal failure, or
reader destruction. A retained ancestor cursor never borrows the record buffer
that the next replay overwrites. Content state for the current file belongs to
[logical content](design-content.md) and is discarded when the walker advances.

## Failure

Argument and state errors detected before work leave the reader unchanged. A
failure in preparation, replay, a driver callback, I/O, a resource limit,
cancellation, or a source change is terminal. The walker invalidates the
snapshot and releases every prepared directory it holds. A partially prepared
directory is released by preparation before the error is returned. Driver
cleanup accepts partial construction and repeated calls.
