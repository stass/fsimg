# Sources and deterministic catalogs

## Ownership and collection

The writer owns one collection session, an append-only entry stream, a host
source-record stream, and a bounded registry of source-root descriptors. A
source root is an opened host directory or regular file paired with the image
path it populates; the CLI opens one root per create operand, so a command
line composes an image from several trees. A root consumes a descriptor
reservation and a charged registry record; neither scales with the number of
files beneath that root. A directory root contributes its tree beneath its
image path and, when that path is not the root, its own directory entry from
the directory's metadata. A regular-file root contributes exactly one file
entry at its image path, which therefore cannot be the root, with the file
as its source. A [manifest](design-mtree.md#a-manifest-is-a-source-root) is
a third kind: its root is the content directory, or a descriptorless root
record that consumes no reservation when the manifest supplies no host
bytes. Programmatic sources
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
before any payload read. A symbolic link becomes a link entry whose source is
its target, read relative to the directory and never followed; special files
fail. Owner and mode are recorded only under the writer's import flag, as the
[POSIX metadata design](design-metadata.md#host-import) states. Directory iteration
retains bounded depth state and accounts for its descriptors and buffers. Native
directory batches have a fixed charged capacity, so libc cannot accumulate a
wide directory outside the memory owner. Host directory order does not determine
catalog order. Regular hardlinks become independent image files. A non-root
image prefix contributes its own explicit directory entry; its ancestors must
be supplied in the same collection. The CLI supplies them as synthesized
directories: entries with a path and no valid metadata field, added through
the same programmatic path as any caller's directory, so the driver's
documented defaults for absent fields apply and host directories the user
did not name never influence the image.

Scanning crosses mount points as tar does: a filesystem mounted beneath a
root, a bind or nullfs alias included, is authorized content of that root.
Each opened object is still checked for its kind and against the output and
scratch identities, on every filesystem. Opening an object can have effects,
such as those of a device node, before its kind is known. Confinement and
device-open limits follow the
[source-root profile](design-security.md#source-root-profile).

Source records contain the root ID, bounded relative host path, device/inode,
size, modification time, and change time. Directory records retain their
identity for validation while scanning. Reopening walks one validated component
at a time, checks opened directories and filesystem identity, and opens only the
current file. A regular-file root has an empty relative path; its source
borrows the root descriptor under the same identity checks instead of
reopening, and the registry keeps ownership. The writer embeds one current
source; its I/O handle owns a reopened file's descriptor. Opening accepts a stable catalog ID, reads and validates the
catalog-owned source fields, and copies the scalar reference before reusing the
source-record buffer for host reopening. A second open while active fails without
disturbing that source. The file is validated when it opens and when its
copy finishes, including empty and zero-only files, not around each bounded
read: a change in between surfaces at finish, and a read that fails, such as
one cut short by truncation, reports a file that changed as changed. Reads
are bounded by the declared length and must be exact. Failure, completion, and destruction
release it. No descriptor or heap object is retained per file. During emission
the alias preflight and the [source copy operation](design-emission.md#source-copy)
are the only callers of source access; format drivers never open, read, or
finish a source.
Ordinary replacements, size changes, and observed change-metadata differences
return `FSIMG_ESOURCE_CHANGED`. These checks are not an atomic snapshot against
a hostile concurrent writer; callers requiring one provide immutable sources
or a filesystem snapshot.

The scanner rejects the context's scratch directory by opened identity, its
device and inode, before descending; an alias that reports another device,
such as a FreeBSD nullfs mount of it, is not recognized. Scratch files are unlinked before source
enumeration can import them; source-tree overlap with the authorized scratch
directory is rejected, rather than excluded by a filename pattern. Output
staging starts after collection. Output/source identity comparisons consume the
host identity records before any sink mutation.

## Catalog preparation

Preparation seals the input and external-sorts entries by component tuples,
with the collection record's rank as the tie-breaker among equal paths. One
bounded path buffer and an ancestor stack assign stable IDs starting at one;
ID zero denotes the implicit root. Every non-root parent must already occur in
tuple order as an explicit directory. Missing parents and file/directory
conflicts fail; equal paths resolve by the merging rule below.

### Merging roots

Several roots can supply the same directory: two operands that both contain
`EFI/`, or a synthesized parent and a scanned directory. The sort makes equal
paths adjacent, and one rule decides each run of equal paths:

* If every record in the run is a directory, the run becomes one directory
  whose children are the union of the children the roots supplied. The
  retained record, and so the directory's metadata, is the one with the
  lowest rank; the others are dropped. A dropped record's metadata never
  reaches the image, so a record that requires a metadata field cannot be
  dropped: such a run is a collision rather than a silent loss, even when
  the kept record carries the same values. Nor can a record of the kept
  record's rank that encodes differently: nothing ranks one above the
  other, so keeping either would discard the other's metadata on an
  arbitrary basis, and the run is a collision.
* Otherwise the run is a collision and preparation fails naming the path: a
  file against a file, a file against a directory, a link against anything.
  The same tree named twice therefore fails on its first file.

The sort key is the component tuple, then the rank, and the lowest rank
wins: the rank is the pair (0 for a scanned record, 1 for a programmatic one
such as a synthesized parent; then the root's registration order, the
operand's position on the command line), so a scanned directory always
outranks a synthesized parent whatever the operand order, and among scanned
directories the first operand wins. Programmatic records share order 0, so
when no scanned record of the path outranks them, programmatic directories
of one path merge only when they encode equally, as the synthesized parents
of several operands do, and otherwise collide. Under a scanned record they
are all dropped whatever optional metadata they carry, so no choice between
them arises. The full encoded record ends the sort key so that the stream
order and the IDs are the same for any insertion order and sort structure.
Within one host root equal paths cannot occur, because a host directory
enumerates each name once; within one manifest root they follow the same
rule, as the [manifest design](design-mtree.md#lines) states. So this is
the only duplicate rule in preparation and it adds no state: the rank is a
fixed-width field of the sort key, and the union of children falls out of
the sort because every child record keeps its own path.

The catalog is independent of operand order except through this rank: IDs,
parent links, generic ordering and every entry's content are the same for any
permutation of operands; only the metadata of a directory that two operands
both scanned follows the command line, and that dependence is documented in
the [CLI contract](design-cli.md#creation).

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
