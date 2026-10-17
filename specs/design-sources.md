# Sources and deterministic catalogs

## Ownership and collection

The writer owns one collection session, an append-only entry stream, a host
source-record stream, and a bounded registry of source-root descriptors. A
source root consumes a descriptor reservation and a charged registry record;
neither scales with the number of files beneath that root. Programmatic sources
use one copied provider table and borrowed argument, with numeric source IDs.
The caller keeps provider state alive until writer destruction. Zero sources
have no backing storage. Directories have no source or payload length.

The common collection constructor and catalog-preparation interface are private
to the library. Public writer construction requires an accepted format writer;
tests exercise common collection directly without registering a synthetic
filesystem or advertising an unavailable format. Collection is sealed before
catalog preparation starts. Preparation failure is terminal, as are collection,
operational source, resource, and cancellation failures. Invalid-state calls and
out-of-range catalog lookups preserve the state. Invalid entry values or provider
configuration are terminal collection failures. Failure immediately closes the
current source and retained host roots, and releases collection-only storage.
A successfully published catalog remains
available for inspection after failure; an incomplete catalog is discarded.
Destruction also accepts partial construction.

Entries contain validated absolute UTF-8 image paths. Root is implicit. Reject
empty components, trailing separators, dot components, invalid scalar encodings,
excess path length, and excess depth. Generic ordering compares component tuples
by unsigned UTF-8 bytes, with a shorter equal component first. Thus `/a`, `/a/b`,
and `/a-` occur in that order. Generic collection preserves spelling and performs
no normalization or case folding. Format-specific case/alias checks consume
the catalog through their own pinned comparison rules.

UTF-8 and UTF-16 codecs accept only Unicode scalar values, reject overlong
encodings and unpaired surrogates, and use explicit lengths and caller-provided
buffers. They neither allocate from an encoded length nor consult the locale.

## Metadata records

Metadata distinguishes validity from requirements. Unknown validity/requirement
bits and requirements without corresponding valid values are invalid. Known
timestamps and generic read-only/hidden/system/archive attributes are retained
without loss in the common catalog. Format planning enforces representability
and the explicit optional-loss policy; collection cannot silently drop a field.
Absent fields serialize as zero, regardless of unused caller struct contents.

Timestamps contain signed seconds, nanoseconds in `[0, 1000000000)`,
a positive precision in nanoseconds, and UTC, known-offset, or unknown-offset
wall-clock basis. UTC and known-offset seconds denote UTC instants relative to
the Unix epoch. A known offset additionally retains minutes east of UTC within
one day. UTC and unknown-offset bases have a zero offset field. Unknown-offset
seconds encode calendar fields as if UTC and do not assert an actual timezone.
Host timestamps are UTC with the host-reported subsecond values and nanosecond
representation; this does not claim that the underlying filesystem clock has
nanosecond accuracy.
No host timezone conversion occurs. Signed values are encoded explicitly rather
than serializing native structs or padding.

## Host scanning and reopening

Scanning is synchronous and descriptor-relative. Each opened object uses
nonblocking, no-controlling-terminal, no-follow flags; its opened type is checked
before any payload read. Symlinks and special files fail. Directory iteration
retains bounded depth state and accounts for its descriptors and buffers. Native
directory batches have a fixed charged capacity, so libc cannot accumulate a
wide directory outside the memory owner. Host directory order does not determine
catalog order. Regular hardlinks become independent image files. A non-root
image prefix contributes its own explicit directory entry; its ancestors must
be supplied in the same collection.

Every opened descendant must have the root's device and mount identity. Linux
uses descriptor-based `statx` mount IDs; FreeBSD and macOS use the mount's unique
`fstatfs` identifier. Unsupported identity queries return an explicit unsupported
result. Post-open identity checking rejects static descendant mounts, including
same-filesystem mount aliases, but does not exclude adversarial concurrent mount
changes or effects of opening a device. Confinement and device-open limits follow
the [source-root profile](design-security.md#source-root-profile).

Source records contain the root ID, bounded relative host path, device/inode,
size, modification time, and change time. Directory records retain their
identity for validation while scanning. Reopening walks one validated component
at a time, checks opened directories and filesystem identity, and opens only the
current file. The writer embeds one current source; its I/O handle owns the file
descriptor. Opening accepts a stable catalog ID, reads and validates the
catalog-owned source fields, and copies the scalar reference before reusing the
source-record buffer for host reopening. A second open while active fails without
disturbing that source. The file is validated before and after bounded reads,
and at completion, including empty files. Failure, completion, and destruction
release it. No descriptor or heap object is retained per file.
Ordinary replacements, size changes, and observed change-metadata differences
return `FSIMG_ESOURCE_CHANGED`. These checks are not an atomic snapshot against
a hostile concurrent writer; callers requiring one provide immutable sources
or a filesystem snapshot.

The scanner rejects the context's scratch directory by opened identity, including
an alias of it, before descending. Scratch files are unlinked before source
enumeration can import them; source-tree overlap with the authorized scratch
directory is rejected, rather than excluded by a filename pattern. Output
staging starts after collection. Output/source identity comparisons consume the
host identity records before any sink mutation.

## Catalog preparation

Preparation seals the input and external-sorts entries by component tuples. One
bounded path buffer and an ancestor stack assign stable IDs starting at one;
ID zero denotes the implicit root. Every non-root parent must already occur in
tuple order as an explicit directory. Duplicate paths, missing parents, and
file/directory conflicts fail.

Sorted records remain in an immutable stream. A paged fixed-record array maps
each stable ID to its stream offset and parent ID. A second stream contains
parent ID, component name, and child ID; external sorting makes exact sibling
collisions adjacent and permits streamed child counts. This stream is the input
for format-specific collision keys and alias assignment. No wide directory or
complete catalog is retained in RAM. Result records, strings, sort runs, and
source references all use the shared metadata owner and the same session.

Successful preparation publishes one immutable catalog. Catalog inspection
copies bounded values into a current snapshot; advancing invalidates borrowed
path pointers. Source access uses a separate charged record buffer and preserves
that snapshot. The shared indexed-read boundary checks stream references,
parent ordering, child-count bounds, and typed entry fields before exposing
values or invoking a source provider. IDs, parent links, metadata, and generic
collision ordering are independent of insertion order, cache capacity, run size,
and merge fan-in.
