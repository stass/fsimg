# EROFS driver

## Scope

The [EROFS format design](design-erofs.md) establishes the on-disk facts,
the profiles and the evidence gates. This document maps them onto code: the
modules of `lib/formats/erofs/`, the driver state behind the common reader
and writer, the algorithms that keep inode lookup, mapping, decoding,
verification, planning and emission bounded, and the tests that qualify
them.

The driver implements the common contracts without exceptions: the
[format descriptor and probe](design-formats.md#format-descriptors-and-profiles),
[prepared directories](design-directories.md) under the portable name
profile, [logical content](design-content.md) with its zero and held
extents, [emission](design-emission.md), [sizing](design-sizing.md), the
[POSIX metadata extension](design-metadata.md) and the
[device-node design](design-device-nodes.md). Common code gains no
EROFS-specific branch. The format has no read settings; its write settings
reach the driver through the write options under the capability record's
`write_settings`.

## Modules

| File | Owns |
| --- | --- |
| `erofs.h` | Private types, constants and prototypes shared by the files below |
| `descriptor.c` | `fsimg_erofs_format`: the `erofs` variant, its capability record and the probe |
| `codec.c` | Pure decoders and encoders of the superblock, configuration records, both inode forms, directory blocks and their entries, attribute headers and entries, long-prefix records, chunk entries, the map header, full records, compact groups and encoded-extent records; the name-filter hash; no I/O |
| `read.c` | `fsimg_erofs_reader_ops`: open, close and volume information; the feature decision |
| `meta.c` | The metadata reader: `erofs_meta_read()` over the primary and box spaces, and `erofs_inode_load()` |
| `map.c` | `erofs_map()`: the one function that maps a logical offset of an inode to a normalized extent |
| `zmap.c` | Compressed lookups: full index, compact groups and encoded extents to one normalized cluster |
| `cluster.c` | The decode windows: stored-byte reads, padding, codec dispatch, plain clusters and the one-cluster reuse rule |
| `directory.c` | `prepare`, `replay`, `keys` and `key`; name order across blocks, dot entries and the parent rule |
| `xattr.c` | Attribute region validation, the extra-data mark, and the pool, prefix and filter checks verification calls |
| `content.c` | `resolve`: mapped, zero and held extents for regular files; the content reader replay and preparation use for symbolic-link targets and directory blocks |
| `verify.c` | Reference and ownership streams, link counts, the packed inode and the box sweep |
| `write-erofs.h` | Plan rows and the plan object |
| `write-plan.c` | `validate`, `plan`, `free`: settings, the entry pass, measurement, placement and sizing |
| `write-dir.c` | Directory block packing, shared by planning (sizes) and emission (bytes) |
| `write-emit.c` | `emit`: inodes, directories, source copies, the box and the superblock |
| `write-lz4.c` | Cluster formation, index records and encoded extents for LZ4 files and the LZ4 box |

`map.c` is the only place that turns a logical offset into storage, and
`meta.c` the only place that turns a NID into bytes, so bounds, layout and
space checks exist once. Reading, verification and the writer's
self-checks all go through them. `write-dir.c` packs directory blocks with
the same arithmetic the parser's rules state, so a block the writer emits
is one the reader accepts.

The driver also needs facilities shared beyond it:

* `lib/core/crc32c.c`: CRC-32C, tested against its published check value;
  the [APFS driver](design-apfs-driver.md) names the same file, and
  whichever driver lands first adds it.
* `lib/codec/lz4.c`: a project-owned LZ4 block decoder and a
  destination-bounded encoder, specified in the LZ4 section of the
  [codec design](design-codecs.md) under its clean-room procedure.
* MicroLZMA and Zstandard block decoding through the optional liblzma and
  libzstd integrations, under the architecture's optional-integration rule
  and specified in the compression libraries design's
  [EROFS block decoding](design-compression-libs.md#erofs-block-decoding).
  Each library's one source file has a private entry point that decodes
  one bounded input into one bounded output under the context's allocation
  adapter, reached through the library's registry row and absent, failing
  as unsupported naming the library, when the build lacks the library or,
  for MicroLZMA, its liblzma predates 5.4.
* Raw DEFLATE through the system zlib under the existing
  [allocation adapter](design-binary-primitives.md).
* A measured read and an encoded copy, for the LZ4 writer
  ([below](#lz4-planning-and-emission)), common operations of the
  [emission design](design-emission.md).

## Codecs

Every EROFS integer is little-endian and decoded with the shared
[scalar codecs](design-binary-primitives.md) after its span is proved.
Decoded records are C structures with `_t` typedefs holding validated values
only:

| Type | Fields |
| --- | --- |
| `erofs_super_t` | Block shift, feature words, root NID, inode count, epoch and nanoseconds, block count, metadata and attribute blocks, UUID, name bytes, algorithm mask or LZ4 distance, prefix count and start, packed and box NIDs, build time |
| `erofs_config_t` | Per algorithm: present, maximum distance, maximum cluster blocks, dictionary size, window |
| `erofs_inode_t` | NID, space, body offset, fixed size, attribute region size, layout, kind, permission bits, link count, size, layout word and its high bits, owner, group, time, serial, layout-bytes offset, body length |
| `erofs_chunk_format_t` | Chunk bits and size, index entry width, wide block numbers |
| `erofs_chunk_t` | Block number or hole |
| `erofs_dirblock_t` | A directory block's bytes, valid length and entry count |
| `erofs_dirent_t` | Child NID, name span, file type |
| `erofs_xattr_header_t` | Name filter, shared count, inline entries' start |
| `erofs_xattr_t` | Entry span, name index, suffix and value spans |
| `erofs_prefix_t` | Record span, base index, infix span |
| `erofs_zheader_t` | Fragment offset or tail size, advice, algorithms, cluster bits, whole-fragment mark, extent count |
| `erofs_cluster_t` | The normalized cluster: logical start and length, source (volume, inline body or packed inode), physical byte start and stored length, algorithm or plain, interlace offset, partial mark |
| `erofs_extent_t` | The normalized extent `map.c` returns: logical start and length, kind (stored, zero or cluster), physical start of a stored extent, the cluster and the skip into its decoded bytes of a cluster extent, and the fragment mark |

Encoding is the exact inverse and writes every reserved byte as zero. The
superblock checksum helper takes the final superblock-region bytes and the
block size and applies the [covered range](design-erofs.md#superblock-checksum);
it is the one place that rule lives.

## Probe and open

The probe reads the 1152-byte prefix and decides identity and the base
superblock's checks, as the format design states. It allocates nothing.

Open:

1. Reads the
   [superblock region](design-erofs.md#volume-identity-and-geometry) from
   `[1024, min(4096, volume end))`, at most 3072 bytes, rereads the
   superblock from it, and checks every superblock rule in this order:
   the probe's checks, the extension slots against the bytes read, the
   extra device count, the checksum, the remaining base fields with their
   feature pairings, the metadata-box slot and the configuration records;
   reserved bytes are recorded and the volume extent follows. A nonzero
   device count therefore refuses the volume before open's feature-pairing
   and chunk rules
   ([external devices](design-erofs.md#external-data-devices)). Open sets
   the volume information's `volume_bytes` to `blocks * B` when the block
   count is nonzero, so the reader core derives `missing_bytes` for a
   truncated volume, and sets the address domain and
   `reader->content_domain`. A truncated volume opens; nothing in open fails
   on truncation unless a structure it reads lies past the end.
2. Decides the features once from the decoded words: the bits this stage
   of the reader refuses fail here with the feature named (the probe's
   checks already refused bits outside the format's table). The decision is
   a set of booleans the rest of the driver reads, never the raw words.
3. Under the metadata box, loads the box inode from the primary space,
   requires a regular file that is flat or compressed behind a full index
   or, with `48BIT`, encoded extents, and that uses no fragment, and keeps
   it, with its validated map parameters, for the life of the reader. The
   box does not require `48BIT`: the root NID comes from the field the
   superblock's form selects and may lie in either space, and a box of
   encoded extents without the bit fails the feature pairing like any
   other inode ([metadata box](design-erofs.md#metadata-box)).
4. Under fragments, loads the packed inode the same way and checks that it is
   a regular file that uses no fragment.
5. Loads the root inode and requires a directory.

Open reads nothing else: no directory, no attribute pool, no prefix table.
Volume information is filled from the superblock and the feature decision.

## The metadata reader

`erofs_meta_read(reader, space, offset, length, out)` returns `length`
bytes at `offset` of one metadata space into a caller buffer. For the
primary space it is one bounded read inside the domain. For the box space
it maps `offset` through the box inode with `erofs_map()` and copies from
the mapped storage or the box window, iterating over at most the
clusters the range spans. A box read never maps through another box read:
`erofs_map()` of the box inode only touches the box inode's body, which open
loaded from the primary space, so the recursion depth is one by
construction.

`erofs_inode_load(reader, nid, out)` computes the space and offset from the
NID, reads the fixed part, decodes and validates it, computes the body
length from the attribute count and layout, and reads the layout header the
length needs. A compressed inode's map header is decoded and validated
here and kept in the inode, so no lookup reads it again and the tail size
and fragment offset it holds are checked once. It records the body's
span and checks it against the space and the inline-tail rule. Later
readers of the body, such as `xattr.c` and `zmap.c`, call
`erofs_body_read()` for the ranges they need, which refuses a range outside
the body as `FSIMG_ECORRUPT` and reads through `erofs_meta_read()`, so no
body is held whole and a body's length is bounded only by the address
domain.

## Mapping

`erofs_map(reader, inode, offset, out)` takes `offset < size` and returns
the normalized extent containing it:

* **Flat**: one stored extent of the full blocks, and for flat-inline a
  cluster extent for the tail: a shifted plain cluster whose source is the
  inode's body, at the tail's absolute offset. There is no separate inline
  kind; a tail is a cluster that is not in blocks.
* **Chunk-based**: reads the one index entry for `offset`, and returns a
  zero extent for a hole or a stored extent of the chunk's bytes.
* **Compressed**: asks `zmap.c` for the cluster containing `offset`, and
  returns a cluster extent. A compressed inline tail is a cluster whose
  source is the inode's body, decoded like one in blocks. A fragment is a
  cluster whose source is the packed inode, at the fragment's offset in the
  packed inode's content: the mapper maps that offset with `erofs_map()` on
  the packed inode and returns the packed inode's extent, clipped to both
  the fragment and that extent, with the fragment mark set and, for a
  cluster extent, the skip from the start of the packed inode's cluster to
  the fragment's first byte there. A whole-file fragment is the same with
  the fragment starting at the file's offset 0. Open refuses a packed
  inode that uses a fragment, its map header fixed from then on, so this
  nests once by construction; the inner `erofs_map()` refuses one again.

Every returned extent is checked once here: its storage inside the domain
or the box's content, its logical range inside `[0, size)`, and a cluster
extent's stored and decoded lengths inside the
[cluster limits](design-erofs.md#limits).

`zmap.c` reads only the records a lookup needs: the record at
`floor(o / L)`, the records its backward distances name, the record after
the head for the count marker and the records after it up to the next head,
each walk bounded by the decoded-extent limit and the record count. Full
records are read in windows of 16; a compact lookup reads the one group
holding a record, computing group boundaries from the record count and the
header's offset; an encoded-extent lookup binary-searches the records,
their count read from the header for 16- and 32-byte records and derived
from the size and the header's logical cluster shift for 4- and 8-byte ones
([encoded extents](design-erofs.md#encoded-extents)). A
lookup keeps the last group or record window it read, decoded and
validated whole, so sequential mapping reads each index byte once.
The compact group decoder is a pure function in `codec.c` from a group's
bytes and its position to the normalized records, and is tested at every
boundary E8 names.

## Decode windows

`cluster.c` owns up to three windows, one per kind of reader, so no reader
evicts another's cluster while it walks: the content window, which
`resolve` hands to delivery as a held extent; the metadata window, which
serves blocks of compressed directories, compressed symbolic link targets,
the packed inode's prefix table and the verifier's decode of the packed
inode; and, under a compressed metadata box, the box window, which serves
box reads. A box read never needs another window's cluster, since the box
inode's map lies in the primary space.

Each window holds one decoded cluster, keyed by the whole normalized
cluster: its source, one of the closed set stored blocks or an inline tail
in an inode's body (a fragment is never keyed, below), its space, one of
the closed set volume or box content, its start in that space's own
coordinates (an absolute volume byte offset, or a byte
offset of the box's content), stored length, algorithm or plain mode with
its rotation, decoded length and partial mark. Every field that changes a
decode's output or its acceptance is part of the key. Two references to the
same stored bytes that differ in any of these, such as a partial reference
and a full one to one shared cluster, are different keys, so a hit always
returns exactly the bytes a fresh decode would. An inline tail, compressed
or plain, fills a window like any cluster and is keyed the same way: a
primary-space tail at its absolute volume byte offset, `meta_blkaddr * B`
plus its body offset, and a box tail at its offset in the box's content. A
fragment is never a key: its bytes are the packed inode's, decoded under
the packed inode's own cluster. No source is
keyed by a relative coordinate, so two keys that are equal name the same
stored bytes.

One fill function is the only writer of a window. It clears the key, fills
the bytes, and sets the key last, only after the length check passes, so a
decode that fails partway, by corruption or by the budget, leaves an empty
window and never a key naming bytes the window does not hold. Decoding a
cluster:

1. reads the stored bytes from the domain or the metadata space into the
   window's input buffer, at most 1 MiB;
2. skips leading zero bytes, since every stream is right-aligned
   ([physical clusters](design-erofs.md#physical-clusters));
3. for a plain cluster, copies or rotates the bytes; otherwise calls the
   algorithm's decoder for the extent's decoded length, partially under a
   partial reference, where the stream continues past the extent and the
   decoder stops without requiring it to end;
4. requires the decoder to produce exactly that length.

The output buffer grows to the decoded length, charged to the context
before allocation, and is released on close. A window whose key matches is
reused without decoding, so sequential reads decode each cluster once and a
delivery that resolves twice inside one cluster costs one decode. Decoders
see one stored stream and one output; they never see an index, a header or
an address.

## Directories

`prepare` decodes the current directory's content block by block through
`erofs_map()`, reading each block, `B` bytes or the last block's valid
length, into one block buffer: by a bounded read for stored extents and by a
copy from the metadata window for compressed ones. For each block it checks
the entry array, the names and their order, carrying the previous block's
last name, at most 255 bytes, across the boundary. It checks `.` and `..`
under the parent rule, using the parent's NID stored in this directory's own
admitted record, and admits every other entry with
`fsimg_directory_admit()`. The admitted record holds the child's NID, its
file type and the current directory's NID, which becomes the child's parent
NID if the child is a directory. Under an omitted dot, `prepare` treats `.`
as present.

`replay` decodes the record, loads the child inode with `erofs_inode_load()`,
checks the file type against the mode, checks the kind's size and layout
rules, validates the attribute region through `xattr.c`, and fills the
entry: kind, size, owner, group, permission bits, the time by `MTIME`,
device numbers and the extra-data mark. For a symbolic link it reads the
target through `erofs_map()` into the common link-target buffer.

`keys` and `key` use the raw name as the namespace key through the common
portable-profile functions.

## Content

`resolve` maps the requested offset and returns:

* a mapped extent for stored flat, chunk and plain shifted bytes in the
  primary volume;
* a zero extent for a chunk hole;
* a held extent from the content window for a compressed or interlaced
  cluster and an inline tail, from the skip on for a fragment's cluster of
  the packed inode; an inline tail fills the content window like any
  cluster, so delivery never reads from the metadata window, whose contents
  another lookup may replace.

Held bytes stay valid until the next `resolve` or walk advance, as the
content contract states. A symbolic link never reaches `resolve`: replay
declares its checked target as decoded content. A file's allocation is
validated when its content is first resolved, not when its entry is
listed, so a damaged flat file lists, and salvage keeps none of its stored
blocks, since the mapper checks their range whole.

## Verification

Verification is the common walk with the driver's checks switched on, plus
streams in scratch: one reference stream and one claim stream, and a second
claim stream under the metadata box:

* **References**: one name record per directory entry other than dot
  entries, holding the child NID, whether it is a directory, whether its
  inode is compact, and, from replay, the child's link count, plus one
  subdirectory record naming the parent for each entry that names a
  directory; the root contributes its own name record. After the walk the
  stream is sorted by NID, so each inode's records are adjacent: a
  directory with more than one name, a directory whose link count is not
  two plus its subdirectory records (outside the compact exception), or a
  non-directory whose link count differs from its names is a finding.
* **Claims**: one record per claimed range with its class, start, length
  and owning NID. The verifier makes them itself, so no reading path
  carries verification hooks: the superblock region and the long-prefix
  table before the walk, then each inode's body, directory blocks or data
  extents through `erofs_map()`, and, through `xattr.c`, its shared
  entries. Inode bodies are exclusive metadata. Box claims go to a second
  stream with the same layout. Each stream is sorted by start, then
  length, class and owner, and swept once with the
  [ownership rule](design-erofs.md#verification), keeping the furthest end
  each class of claims has reached: exclusive, shared and data. An
  exclusive claim overlapping any other claim, a data claim overlapping a
  shared one and a shared claim overlapping a different shared range are
  findings. The helpers' ownership runs
  ([verification helpers](design-verification-helpers.md)) are not used:
  they reject every overlap, so they could check only the exclusive claims,
  which the one sweep checks anyway.

Each inode is recorded and read once. The walk claims a directory's body,
blocks and attributes where it reaches the directory, which has one name;
a directory reached twice fails the reference check, which runs before the
sweep. A non-directory contributes only its name records to the walk. The
pass over the sorted references, where all names of an inode are adjacent,
loads each non-directory once its link count has matched its names, claims
its body, storage and attributes, and under payload verification reads a
regular file's content through `erofs_content_read()`, charging a unit of
work per 64 KiB batch. The payload count adds the bytes delivery would map
in the volume: those of stored extents and of plain clusters stored
shifted in blocks, never decoded, held or zero bytes. A batch that starts
in a chunk hole skips the whole hole for one unit of work and reads
nothing, so a chunk index of a few entries describing 2^43-byte holes
costs its entries, not its length. A file with N names is therefore read
once and claims its extents once, and a file whose link count is wrong is
never read, so a hostile volume whose directory blocks all name one large
file costs one name record per entry and no payload reads. This bounds
names, not bytes: data claims may overlap, so many inodes, or one chunk
index, mapping the same large region still read it once per mapping,
which the session's work and time limits bound. The reference stream is
the visited set: nothing else is spooled, and no inode claims its body
twice. Payload is read in NID order, not walk order. QNX6 reads a file at
its first name and skips later ones; EROFS has no first name, so the
sorted references stand in for it.

`verify.c` also maps and decodes the packed inode's and the box's every
cluster once, claiming their storage, whether or not a reachable file uses
it. A file's fragment extents claim nothing: the bytes are the packed
inode's, and mapping them proves the fragment range inside the packed
inode's content. An entry naming the packed inode is a finding, since the
packed inode is outside the namespace. A codec the build lacks, a nonzero
reserved byte of the superblock region, an unknown compatible bit and name
filters the superblock's filter byte turns off each mark the result
incomplete with a reason: verification fails `FSIMG_EUNSUPPORTED` naming
it. The three volume-wide reasons are reported after every structural
check has passed, so a volume that is also corrupt reports its
corruption; a missing codec, like an unknown attribute index, fails where
the walk meets it.

## Writer settings

| Setting | Field | Driver rule |
| --- | --- | --- |
| `cluster=` | `allocation_unit_bytes` | 0 or an accepted block size |
| `-s` | `volume_bytes` | 0 or a multiple of 512 holding the plan's blocks |
| `-L` | `label` | At most 15 bytes |
| `uuid=` | `volume_uuid` | Verbatim; unset in reproducible mode, the identifier rule's EROFS role; unset otherwise, `FSIMG_EINVAL` naming the key, and the CLI supplies a random version 4 UUID |
| `-e` | `epoch` | Any signed 64-bit second count; the format time in reproducible mode |
| `compression=` | `erofs_compression` | `none` or `lz4` |
| `addressing=` | `erofs_addressing` | 32 or 48 |
| `metabox=` | `erofs_metabox` | `none`, `plain` or `lz4` |

The last three are format-specific write settings in the
[settings catalogue](design-settings.md), refused by every other format.

## Planning

Planning is a fixed sequence of passes over the sealed catalog, each
sequential, with plan rows in a scratch-backed paged array indexed by
catalog position:

1. **Settings and time.** Validates the settings and fixes `B`, the format
   time and, under 48-bit addressing, the epoch from one pass over the
   catalog's modification times.
2. **Entries.** For each catalog entry in order: kind, size, owner, mode,
   time, link count, and for a directory its size from `write-dir.c`, which packs
   the sorted names, with `.` and `..` merged into their byte-order places,
   without needing any NID. A subdirectory adds a link to its parent's
   row, so a directory's link count is final only once its children have
   been visited.
3. **Measurement**, under `compression=lz4` only: for each regular file
   whose flat layout stores at least two blocks, since a compressed file
   stores at least one, a [measured read](#lz4-planning-and-emission)
   encodes the content, records its cluster count in the row and appends
   its cluster records to a scratch store; the row chooses the compressed
   layout when it stores fewer blocks, and the store keeps only the chosen
   files' records. No address exists yet, so a cluster's block is its
   position among the file's clusters; emission and box generation add the
   file's planned start block when they encode it.
4. **Metadata placement.** Fixes each row's inode form and its flat or
   flat-inline layout from its final fields, since an extended inode's
   larger fixed part decides whether a tail fits, then assigns its body
   offset, and so its NID, with the block rules of the
   [layout](design-erofs.md#layout), and
   fixes the metadata area's end. Under the box the offsets are in the box's
   content and the primary area holds the box inode alone, or the box inode
   and the root under `addressing=32`
   ([metadata-box profile](design-erofs.md#metadata-box-profile)); under
   `metabox=lz4` the box inode's record list is sized for `ceil(content / B)`
   records, the full index's exact count under `addressing=32` and the
   cluster former's upper bound for encoded extents under `addressing=48`,
   so the area's end does not wait for the box's encoding.
5. **Data placement.** Assigns each row's start block or first cluster
   block in order, then the box's blocks after the data, and fixes the
   block count and volume length. Under `metabox=lz4` the box's cluster
   count comes from a measured encoding of the box content, which pass 5
   can generate because every NID and start block it holds is now final.
6. **Sizing.** Checks `volume_bytes`, the 32-bit limits without
   `addressing=48`, and the output limit.

Every row's encoded length depends only on the catalog, the settings and,
for an LZ4 file, its measured clusters, all known before pass 4, so no
later pass changes an earlier one's result.

## Emission

Emission runs in plan order after the common writer has zeroed the volume:

1. For each row: encode the inode and its layout bytes into the metadata
   area, unless the box holds it; for a directory, generate each block with
   `write-dir.c`, filling child NIDs from the plan rows, and write it; for a
   file or link, issue one source copy over its regions: its full blocks,
   then its inline tail in the body; for an LZ4 file, an encoded copy.
2. Under the box: the box content, every body in plan order, is generated
   from the plan rows and the cluster records one block at a time and written
   as the box's stored blocks, through the LZ4 cluster former under
   `metabox=lz4`; then the box inode is written in the primary area.
3. The superblock, then the checksum over the final bytes of its covered
   range. Emission does not read the sink back: it keeps its own copy of
   the covered range, into which every metadata write and, through the
   copy observer, every source byte that lands there is mirrored, so the
   copy holds the range's final bytes once the last inode is written.

## LZ4 planning and emission

Compression is the one place the writer's layout depends on payload bytes.
Two common operations of the [emission design](design-emission.md) keep
the source lifecycle common:

* A **measured read** during planning opens a catalog entry by its stable
  ID, streams its content in bounded batches to a driver observer, and
  closes it with the same identity validation as a copy. The observer runs
  the encoder and yields the cluster count and each cluster's index record;
  it keeps no payload bytes.
* An **encoded copy** during emission streams the same content to a driver
  encoder, which returns each finished cluster's bytes for the emission
  object to write at the planned position. It fails as a source change when
  the encoder's clusters differ from the plan's.

`write-lz4.c` holds the cluster former: the destination-bounded LZ4 encoder
fills one block with the longest input prefix whose encoding fits, emits a
plain cluster when one block of input does not, and records each head's
logical offset. The full-index records, or under 48-bit addressing the
encoded extents, follow from the heads, for the LZ4 box as for a file.
Measurement keeps one fixed-size
record per physical cluster in a scratch store: its logical start, the
logical bytes it holds, its stored length and whether it is plain, which
is metadata, not payload; a cluster's position among the file's records is
its block relative to the file's first block. Emission encodes each
body's index from those records through `codec.c`, which writes every
reserved byte as zero, and the encoded copy checks that every cluster it
forms matches its record in all four fields, so emission holds one
window buffer, one output block and the current record. Every byte of the
output block is written for each cluster: the encoder's bytes are moved to its end and the gap before them
zeroed, or the block is zeroed before a plain cluster's bytes are copied
to its start, so the padding of a right-aligned LZ4 cluster and the end
of a short last cluster are zero, never bytes of the previous cluster or
another file; under `LZ4_0PADDING`
the decoder depends on that zero run to find the stream's start. Keeping the
records is what lets the box, whose bodies hold them, be generated without
reading a source. The box under `metabox=lz4` uses the same former on
generated bytes. No payload is staged: planning and emission each encode
once, and the encoder's determinism makes the two agree.

## Resource bounds

| Use | Bound |
| --- | --- |
| Open | The superblock region, at most 3072 bytes |
| Inode load | The fixed part and the headers it needs; bodies read in ranges |
| Directory preparation | One block buffer and the previous name; children spooled by the common walker |
| Index lookup | One full record window, one compact group, or one encoded-extent record per search step |
| Decode windows | Two, three under a compressed metadata box; each at most one decoded cluster plus a 1 MiB input buffer and the codec's state, the MicroLZMA dictionary or Zstandard window the [configuration record](design-erofs.md#configuration) declares included, charged before allocation |
| Verification | The reference and claim streams in scratch, three under a metadata box, under the common sort's run buffer |
| Planning | Plan rows in a paged array; directory packing of one directory's names at a time, streamed from the catalog; LZ4 cluster records in a scratch store; for the file being measured, the measured read's 64 KiB batch buffer and the cluster former's window buffer and output block |
| Emission | One directory block and its name staging block, a copy of the checksum's covered range of at most 3072 bytes, an LZ4 window buffer of two 64 KiB windows and an LZ4 output block, one box staging block |

Work is charged per block read, per index record visited and per decoded
byte, so an image whose distances force long backward walks, or whose
directories alias one subtree many times, exhausts the work budget rather
than running unbounded. The writer charges each LZ4 cluster's input bytes
when it forms the cluster, once in measurement and once in emission, so a
work budget bounds compression too.

## Failure classification

The driver reports the format design's
[classes](design-erofs.md#failure-classification). `codec.c` and `zmap.c`
return `FSIMG_ECORRUPT` or `FSIMG_EUNSUPPORTED` with a static phrase naming
the field; `cluster.c` maps a decoder's rejection to `FSIMG_ECORRUPT`, and a
missing library or an LZ4 cluster on a volume without `LZ4_0PADDING` to
`FSIMG_EUNSUPPORTED`; budget errors pass through
unchanged. `prepare` and `replay` report decoder rejections under
`FSIMG_OP_READER`, so salvage can step over them.

## Tests

| Program | Covers |
| --- | --- |
| `test/erofs-gen.c`, `test/erofs-gen.h` | An independent image generator written from the format design, sharing no code with `lib/formats/erofs/`: superblocks with checksums, both inode forms, every layout, directory blocks, attributes, chunk indexes, full and compact indexes with clusters encoded by a test-only LZ4 literal encoder, 48-bit fields, encoded extents and boxes; and per-field mutation |
| `test/erofs.c` | Probe, open, every field rule, mapping at every boundary, compact groups at every boundary, directory rules, the parent rule, attributes, content through common delivery, every failure class |
| `test/erofs-verify.c` | Reference and ownership findings, incomplete reports |
| `test/erofs-write.c` | Round trips through the reader and verifier; byte identity across source order, budget, scratch and time zone; poison scan; every profile and combination |
| `test/erofs-faults.c` | Allocation, short-read, write and flush faults at every stage of open, read, verification, planning and emission, each followed by a cleanup check |
| `test/codec.c` | The LZ4 decoder and encoder: published vectors, round trips, truncated and hostile streams, and the encoder's bound |
| `test/driver-contracts.c` | The `erofs` profile in the common contract suite |
| `fuzz/erofs.c` | Probe, open, mapping and directory parsing with checksum repair, under finite limits |
| `test/erofs-integration.sh.in` | Producer fixtures from the pinned `mkfs.erofs` read and compared with their source manifests; writer output checked by `fsck.erofs --extract`, `rust-fs-erofs` and, in the Linux check environment, a kernel mount; each skipped explicitly when absent |
| `test/spec/lib/fsimg_spec/erofs.rb` and `test/spec/suite/erofs/` | The specification harness's Ruby model of the core profile: superblock, checksum, inodes, flat data and directories, with rule-derived valid and invalid images |
