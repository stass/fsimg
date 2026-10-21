# Image layers: schemes, containers and compression

A filesystem volume rarely travels alone. It sits in a partition of a disk,
the disk is stored in a disk-image file format, and the file may be
compressed. This design defines how fsimg reads and writes those layers around
the volume without teaching any filesystem driver about them.

Terms:

* **Volume**: the bytes a filesystem driver reads or writes. Every existing
  driver contract applies to a volume unchanged.
* **Disk**: the bytes a partition **scheme** (GPT, MBR) describes. A disk
  without a scheme is a bare volume.
* **Container**: a disk-image file format that stores a disk as chunks with
  its own tables and checksums: DMG/UDIF, fixed VHD, Android sparse image
  (simg). "Container" alone always means this. The APFS container, the APFS
  structure that holds APFS volumes, is always written out as "APFS container"
  and is out of scope here.
* **Compression**: a whole-file encoding (gzip) around any of the above. It is
  not a layer slot; it is a property of the file.
* **Stack**: the ordered description of the layers around one volume, for
  example `fat32/gpt/dmg`.

The [architecture](architecture.md#contracts) owns the shared contracts this
design relies on: the resource context owns budgets, bounded I/O owns byte
ranges, and the output coordinator owns publication. The
[storage design](design-storage.md) states the bounds of the container
adapter, chunk index, spool and transcode. The [C API](design-api.md) declares
the public objects. The [security design](design-security.md) owns the worker
and sandbox model; the [Security](#security) section below states what the
layers add to it.

## Owners

| Owner | Owns |
| --- | --- |
| Stack parser (`lib/layers/stack.c`) | The three-slot representation, its text syntax, canonical printing, and the layer registry |
| Layer descriptor, one per layer | Probe, read-side open (peel), write-side plan/encode/transcode, verification, settings and capability record |
| Image (`lib/layers/image.c`) | Read-side peeling order, detection and forcing, partition enumeration and selection, the spool, the layer verification report |
| Container adapter and chunk index (`lib/layers/chunks.c`, `lib/meta/`) | Disk-offset to chunk resolution, the decompressed-chunk cache, chunk validation |
| Disk plan (`lib/layers/plan.c`) | Disk sizing around a volume, partition placement, partition types and names, identifiers, the plan's disk and partition report |
| Layered output (`lib/layers/output.c`) | The scheme tables written into the staged raw disk, the assembled disk, the transcode pass, the sequential sink, raw-intermediate lifetime |
| Output coordinator (`lib/output/`) | Staging directory, publication, durability, unchanged |
| Filesystem drivers | The volume only; they never see a layer |

Each obligation has exactly one owner. A driver cannot reach a container or a
partition table, and a layer cannot reach a filesystem driver except through
the bounded probe used for partition auto-selection and default partition
types.

## The stack

An image is a stack with three slots in fixed order, each at most once:

| Slot | Kind | Names |
| --- | --- | --- |
| 1 | Filesystem | `fat12`, `fat16`, `fat32`, `exfat`, `qnx6`, `qnx4`; reserved: `apfs`, `hfs` |
| 2 | Scheme | `gpt`, `mbr` |
| 3 | Container | `dmg`, `vhd`, `simg` |

The text form is `fs[/scheme][/container]`: the present slots inner to outer,
separated by `/`, the same order as file suffixes. A token is a layer name or
`-`. Every name belongs to exactly one kind and fills the slot of that kind;
`-` fills the next slot in order and marks it empty. Each token must fill a
slot later than every slot filled before it, so parsing is one rule: split on
`/`, look each token up, place it, and reject a token that does not advance.
A repeated kind, a name of a kind already passed (`fat32/-/gpt`), an unknown
name, a `-` in the filesystem slot, an empty token and an empty string are
syntax errors naming the token. The parsed representation is the three slots,
by construction, each holding a name, an explicit empty (`-`) or nothing;
there is no list to validate afterwards.

`fat32/gpt/dmg`, `fat32/dmg` (no scheme: `dmg` can only be a container),
`fat32/-/dmg` (the same, spelled out), `fat32/-` (no scheme, container
unspecified) and `fat32/-/-` (a bare volume with nothing around it) are all
legal. What an unfilled slot means depends on the operation. On create it is
empty, so `fat32/dmg` and `fat32/-/dmg` write the same image. On read it is
detected and only `-` forces it empty, so `fat32` unwraps whatever container
and scheme surround the volume while `fat32/-/-` forces every slot: the
file is a bare FAT32 volume, decompressed first only when the
[step-1 rule](#peeling-order) finds gzip alone claiming it. A stack with an
empty filesystem
slot is legal syntax (`gpt`, `dmg`, `gpt/dmg`); which operations accept it is
stated with each operation. A stack describes one filesystem in one
partition; multi-partition disks exist only through
[compose](#wrap-unwrap-and-compose).

The canonical text, which `i` prints, describes a fully known stack: it omits
empty slots and `-`, because every name says which slot it fills, so a
detected `fat32/-/dmg` prints as `fat32/dmg`. Pasted into `-F` on create it
produces the same stack. Compression is reported beside it (`gzip`), never
inside it.

### Layer settings

A setting without a layer qualifier targets the filesystem: `oem`,
`sector`, `cluster`, the QNX6 `endian`, `uuid`, `inodes`, `reserve`, and the
QNX4 `inodes`.
A layer setting is qualified by its layer name: `gpt:type=esp`,
`dmg:compress=none`, `gzip:level=9`. A qualified key for a layer absent from
the stack fails; fsimg never guesses the target of a bare key. On read the
stack is not known until detection runs, so a read-side key is accepted only
for a layer the forced stack names: `gpt:sector=4096` needs `fat32/gpt` or
`gpt` in the stack, and a key for a layer left to detection fails before any
image byte is read. The per-layer keys, types, defaults and validation are in
each [profile](#layer-profiles), summarised in the [settings table](#settings).
In the library every layer setting is a typed field of
`fsimg_layer_options_t`; a layer's capability record declares the settings it
stores, and a field for a layer absent from the stack must hold its default,
the rule already applied to filesystem settings.
On creation a key of a layer with no write support, a container or a
compression, is accepted without being read until that layer is written; a
key of a scheme is validated against the [settings table](#settings).

## Layer contract

A layer descriptor is a static record, registered in a static list exactly as
format descriptors are. Its operations form a closed set per kind:

| Operation | Scheme | Container | Compression |
| --- | --- | --- | --- |
| Probe | Claims a disk from its prefix | Claims a file from its prefix and its last 512 bytes | Claims a file from its prefix |
| Peel (read) | Enumerates partitions as checked slices of the disk | Yields a read-only positional I/O object over the disk, backed by the chunk index | Decodes a sequential stream into the spool |
| Plan (write) | Derives the disk length and the volume offset from the volume length and the container granularity | Declares its disk-length granularity | None |
| Encode (write) | Writes its tables at fixed disk offsets through bounded metadata writes | Transcodes a disk sequentially into the sink | Wraps a sequential sink |
| Verify | Table checksums, consistency, partition bounds and overlap | Container checksums, chunk tiling and bounds | Member checksums and length, at spool time |
| Capability | Settings, read/write flags, sector sizes | Settings, codecs, codec version | Codec version |
| Suffixes | None | The file suffixes that name it: `.dmg`, `.vhd`, `.simg` | `.gz` |

Probes follow the [bounded probe contract](design-formats.md#format-descriptors-and-profiles):
pure functions of fixed bytes and the input length, no I/O, no allocation,
and a claim only after an identity signature matches. A container probe
additionally receives the last 512 bytes, because DMG and VHD identify
themselves at the end of the file, and uses the input length to tie that
trailer to the file: a trailer whose ranges do not end at the file's end is
not a claim, so a bare volume whose last sector happens to hold one is not
taken for a container. Common code reads the prefix and the tail once per
peel.

The registry is the single source of layer suffixes. The CLI reads them from
the descriptors for three uses, and a container added to the registry is
covered by all three without a second list: stripping an image name to its
extraction stem; choosing the compression under `-a`, which selects
compression from the suffix and never a container; and the `-a` conflict
check between an output name's container suffix and the stack's container.
A reserved compression name (`zstd`, `xz`) is registered with its suffix
but no profile: its suffix is stripped like gzip's, and `-a` selects it so
that creation fails as unsupported rather than writing an uncompressed
image under a compressed name.

The read-side result of every peel is an ordinary `fsimg_io_t`: a container
yields a positional object over the disk; a scheme yields slices of it. A
filesystem driver opens a slice and knows nothing else. The write-side
result of every scheme is a placement; the write-side result of every
container and of compression is a sequential sink. Nothing else crosses a
layer boundary.

## Reading

### Peeling order

Opening an image peels from the outside in, and each step produces the input
of the next:

1. **Compression.** The compression probes run on the input's prefix and,
   when one claims, so does every other probe that can run there: the
   scheme and filesystem probes, and for a seekable input the container
   probes. This step decides one thing, whether the input is compressed,
   and decides it here and nowhere else: one compression claims and
   nothing else claims, compressed by it; a compression and any other
   probe, another compression included, claim, ambiguous, which an
   explicit read-side stack (one naming or emptying any slot) resolves as
   not compressed; otherwise not compressed. A forced compression, as `-z`
   forces gzip, runs only its own probe, which must claim. A stream's first block is probed as the start
   of a file of unbounded length, so a probe's check against the length
   passes rather than hiding a claim, and an ambiguity is reported instead
   of a silent choice. A claim other than a compression's at this step
   decides nothing further; containers, schemes and
   filesystems are decided by their own steps below, so a fixed VHD whose
   raw disk also draws a filesystem claim is not ambiguous here. A
   compressed input is decoded into the [spool](#stream-input-and-the-spool);
   a non-seekable input that is not compressed is copied into the spool
   unchanged, because its container trailer cannot be reached otherwise. The
   spool is then the file. A seekable uncompressed input is the file as it
   is.
2. **Container.** The container probes run on the file's prefix and tail.
   No claim means the file is the disk. One claim peels: the container's
   tables are validated and the disk becomes a positional object backed by the
   [chunk index](#chunk-index-and-cache). The DMG koly trailer, VHD footer and
   simg header all state the disk length, so the disk object has a fixed
   length before any volume byte is read.
3. **Scheme or bare volume.** The scheme probes and the filesystem probes run
   on the disk's prefix together. A scheme claim enumerates partitions; a
   filesystem claim means a bare volume occupying the whole disk.
4. **Volume.** The selected slice is opened by `fsimg_reader_open()` exactly
   as a raw image.

### Detection and forcing

Steps 2 and 3 apply the format-detection rule: exactly one claim decides; no
claim at the container step means no container, and at the scheme step an
unrecognised image (`FSIMG_EFORMAT`); several claims are ambiguous and
require a forced slot. Step 1 has its own outcome rule, stated with the
step. Probes claim on **identity**: the signature, the format's fixed
constants where the [profile](#layer-profiles) names them (a version, a
header size, a method byte) and, for a trailer, its tie to the input length.
Everything a profile validates after the claim, checksums, table contents
and declared geometry beyond that tie, makes a claim `FSIMG_ECORRUPT` or
`FSIMG_EUNSUPPORTED` and never falls through to another decoder, so a GPT
with a damaged header CRC is reported as a damaged GPT, not read as a bare
volume, and a dynamic VHD is reported as unsupported, not as a raw disk with
an odd tail. Running scheme and filesystem probes together at step 3 is what
makes a FAT boot sector whose code region happens to resemble a partition
table an explicit ambiguity rather than a silent misread.

A read-side stack forces the slots it names, forces empty the slots it marks
`-`, and leaves the rest to detection. `fat32` forces the filesystem decoder
but still unwraps a DMG and a GPT around it; `fat32/-/-` forces the whole
stack. A forced layer runs only its own probe and open with full validation;
an absent signature is `FSIMG_EFORMAT`. Forcing a filesystem narrows the
step-3 filesystem probes to that one; `-` in the scheme slot removes the
scheme probes; `-` in the container slot skips step 2. Forcing never
disables a check.

Compression is not a slot, so no stack forces it: the
[step-1 rule](#peeling-order) decides it whether or not a stack is given,
and an explicit read-side stack only resolves a step-1 ambiguity. The image
options force a compression by its registered name instead, as `-z` forces
gzip; a forced compression runs only its own probe, and an input it does
not claim fails as any forced layer with an absent signature does.

One survey runs every probe a step needs, layers and filesystems alike,
and counts the claims under one rule: a probe's `FSIMG_EFORMAT` is no
claim, and any other verdict, a corrupt or unsupported one included, is a
claim whose first instance is kept with its reason. Each step reads its
outcome from that count, none, exactly one or several, so the scheme,
container, compression and filesystem probes cannot drift apart in what
counts as a claim.

### Partition selection

A scheme yields partitions in table-slot order, numbered from 1 (the first
GPT entry or MBR slot is partition 1, as `sgdisk` and `/dev/sda1` count).
Unused slots keep their number so that numbers match other tools. Selecting a
volume uses one of these selectors:

| Selector | Matches |
| --- | --- |
| Automatic (default) | The one used partition whose prefix draws a filesystem claim, running the forced filesystem probe when one is given and every filesystem probe otherwise. Corrupt and unsupported claims count, so the eventual open reports the real reason. Zero candidates is "no recognised filesystem in any partition"; several is an error that lists them and requires a selector. |
| Index `N` | The partition in slot `N`; an unused slot is an error. |
| Name `TEXT` | GPT only. `TEXT` is validated UTF-8, converted to UTF-16LE code units and compared unit for unit with the entry's name truncated at its first zero unit; no case folding or normalisation. Exactly one entry must match. |
| Type `NAME` | A name from the [type table](#partition-types) or a literal GUID or MBR type byte. Exactly one used partition must carry that type. |
| Range `offset, length` | A checked slice of the disk. Step 3 is skipped entirely: no scheme probe runs, so a damaged partition table does not block it. The escape hatch for unusual layouts. |
| Disk | The whole disk, for unwrap, layer verification and raw access. Like Range, it runs no scheme probe. It is a slice like any other, so `fsimg_reader_open()` accepts it; on a partitioned disk the filesystem probes then meet the partition table and report an unrecognised image, which is the honest result. |

A disk without a scheme, detected or forced empty, has no partition table and
no slot numbers: Automatic yields the whole disk as the volume, Range and
Disk work as on any disk, and Index, Name and Type are errors; the API
reports the volume without a slot. Name with MBR is an error too. The CLI
spells the selectors as `--partition=N`, `name=TEXT` and `type=NAME`, and
the range as `--offset`/`--length`; the [CLI design](design-cli.md) owns the
spelling.

The range selector is relative to the bytes the container yields: for a DMG
it addresses decoded disk offsets, never offsets in the DMG file. Offsets and
lengths must lie within the disk; an explicitly supplied length is positive.

### What the volume sees

The selected slice has a fixed length, retains backing identity for alias
checks through every layer, and translates to the backing object with one
checked base offset when the backing is a file. When the backing is a
container adapter, the slice translates to a disk offset and the adapter
resolves it to a chunk. There is still no recursion: adapters stack at most
two deep, the [assembled disk](#wrap-unwrap-and-compose) of compose above a
container adapter, and a container adapter sits only above a file. Drivers
remain unaware of layers: the reader, the walker, content delivery,
extraction and verification are unchanged.

## Stream input and the spool

A non-seekable input (standard input under `-f -`, whatever kind of file it
is) and a gzip-encoded input (seekable or not) cannot be peeled in place:
container trailers sit at the end and gzip offers no random access. Both are
decoded into a **spool**, a private sparse regular file, and the spool is then
read as an ordinary seekable file with layers detected inside it. Seekable
encodings, such as DMG chunks, are read in place, never spooled.

### Handed-over files

The spool and the [raw intermediate](#staging-and-the-raw-intermediate) are
the two payload-sized temporary files fsimg uses; the
[storage design](design-storage.md#resource-model) places them among the
other resources. Both follow one rule. The trusted side that holds directory
authority creates the file exclusively, empty, with mode 0600, unlinks it at
once, and hands the descriptor to the library, which adopts it on every
outcome: the CLI coordinator does this before its worker starts, an embedding
application in its own code. The library never creates or removes a file by
name for either purpose, so a worker holds a file, never the authority to
make or delete one, and an unlinked file has no name to clean up and cannot
outlive the operation. On adoption the library checks the descriptor before
any byte moves: `fstat` must show a regular file of size zero with link
count zero, and the open mode must be read/write without `O_APPEND`
(which would move every write to the end and corrupt skipped holes);
anything else is
`FSIMG_EINVAL` naming the descriptor. The creator keeps no other descriptor
or mapping of the file and gives none to anyone, so the library's object is
the file's only writer for its lifetime; that is part of the handover
contract, since no check on the descriptor can establish it. The checked
facts, an empty regular file that no name can reach, and the contracted
one, a single writer, are together what the file's
[written-range record](design-holes.md#the-written-range-record) rests on,
so unwritten ranges are proven zero, all-zero blocks are skipped and stay
holes, and the proof of emptiness does not depend on the caller's word or
on the host's report of its holes. The CLI coordinator's placement rules
are in the [CLI design](design-cli.md#coordinator-and-streaming-representation).

### Budget

The spool is a context resource of its own, `spool`, distinct from metadata
scratch: scratch never holds a second image, and the spool never holds
metadata. Its limit is the sum of the logical lengths of live spools in a
context, default **64 GiB**. This default is policy, not a measurement: it
matches the scratch default, covers the disk images people actually pipe
through a tool (SD cards, virtual disks) and bounds a decompression bomb to
the same order as the existing scratch policy. The CLI key is `-l spool=` and
the option field `spool_bytes`; `FSIMG_RESOURCE_SPOOL` identifies it in
errors.

### Filling

* The open adopts the spool descriptor whether or not the input needs it; an
  unused spool costs one unlinked empty inode. Without a spool descriptor,
  stream and gzip input fail explicitly as a spool limit error with limit
  zero.
* The filler decodes the input itself. For a stream it reads the pipe the
  trusted side hands it, runs the step-1 probes on the first block, and
  either inflates or copies the rest; nothing outside the library looks at
  the bytes. Decoded bytes are written in bounded buffers; a buffer block
  that is entirely zero is skipped and stays a hole. The final length is set
  by resize. All-zero input therefore costs no physical storage, while the
  logical length is charged in full.
* Logical length is reserved against the spool limit before each extension,
  in bounded steps. A gzip member whose output would cross the limit fails at
  the step that would cross it, before those bytes are written. gzip's trailer
  length is only a 32-bit modulus and arrives last, so the observed length is
  the bound; a container inside the stream is bounded again by its own tables
  once the spool is complete.
* Members are decoded in sequence, as `gzip(1)` does; each member's CRC32 and
  modular length are checked. Bytes after the last member are an error, not a
  warning. The decoder's state and window are charged to memory.
* Decoding counts work as a container's chunk decoding does: every
  compressed byte the decoder reads, member headers and trailers included,
  and every decoded byte it produces is charged to the work counter, for a seekable file and a stream alike, so
  `-l work=` and the deadline bound a decode, and a stream stops at the work
  limit with `FSIMG_RESOURCE_WORK`.
* Cancellation and I/O errors stop the fill; a partially filled spool is
  discarded with the image.

A spooled stream is read with the same probes and limits as a file. A spool
whose content begins with another gzip signature is an unrecognised image;
compression is peeled once, at the file level, never recursively.

## Writing

### Central construction

Emission stays random-access and the layers stay sequential. The filesystem
writer emits its volume, through the unchanged [emission](design-emission.md)
contract, into a staged raw file: the whole file when no scheme is present,
or a checked output slice of a staged raw **disk** when one is. The scheme
encodes its tables into the same staged disk through bounded metadata
writes. When a container or compression is present, one **transcode** pass
reads the staged disk sequentially and writes the final staged output
sequentially. No driver streams, and no layer seeks in its output.

```text
volume plan -> scheme plan -> staged raw disk (emit volume, encode tables)
            -> [transcode: container encoder -> gzip encoder -> sink]
            -> publication (file) or completion (stream)
```

### Sizing

The filesystem sizes itself first, under the [sizing design](design-sizing.md):
`-s` and `volume_bytes` are the volume size. The scheme then derives the
disk from the volume:

* The volume starts at the first offset that is a multiple of 1 MiB and lies
  beyond the scheme's leading tables. 1 MiB is the alignment every current
  partitioning tool uses, so a disk fsimg writes matches what `sgdisk` or
  `fdisk` would lay out.
* Trailing tables (the GPT backup header and entry array) follow the volume.
* The container, when present, declares a disk-length granularity: 512 bytes
  for DMG and VHD (a sector), the block size for simg, or a larger explicit
  alignment such as `vhd:align=1M`. The disk length is rounded up to it, and
  the scheme places its trailing tables at the end of the rounded disk, so a
  GPT backup header is at the last LBA of the disk the container declares.
  Padding between the volume and the trailing tables is zero and is
  unallocated space in the scheme.

A scheme accepts an exact disk size, `gpt:disk=` or `mbr:disk=`. It is a
constraint like an explicit volume size: with `-s` the volume must fit between
the aligned start and the trailing tables, and the remainder stays
unallocated; without `-s` the volume takes the whole usable range, rounded
down to the filesystem's sector size, and that length is passed to the
filesystem as its exact size. The filesystem resolves its sector from its
options before the fit and validates the fitted length after it, so the fit
rounds to the sector the filesystem writes, whichever it chooses. `disk=` must be a multiple of the container's
granularity and the scheme's sector size. `FSIMG_WRITE_MINIMIZE` minimises the
volume; the scheme adds its fixed overhead and never changes the result.

The plan reports volume length, volume offset and disk length separately, and
the container's granularity padding when nonzero.

### Staging and the raw intermediate

The output coordinator stages unchanged: a private directory on the
destination filesystem, the staged file created exclusively, published
without replacement after the worker exits. The layers add one
[handed-over file](#handed-over-files), the **raw intermediate**, wherever a
transcode or a stream needs a raw disk to read from:

| Stack | Output | Staged file | Raw intermediate |
| --- | --- | --- | --- |
| Filesystem only; filesystem and scheme | File | The raw volume or disk is the staged output | None |
| Filesystem only; filesystem and scheme | Caller-supplied positional sink | None; the sink receives the raw volume or disk | None |
| Filesystem only; filesystem and scheme | Stream | None | The raw volume or disk, copied sequentially to the stream |
| Container or compression present | File | `image`, written by the transcode | The raw disk the transcode reads |
| Container or compression present | Stream or caller-supplied positional sink | None | The raw disk the transcode reads |

The raw intermediate is charged against the output limit together with the
staged output, never against scratch, whichever directory holds it: while a
transcode runs, the image costs (disk length + encoded output length) of
output budget, and the [architecture](architecture.md#bounded-resources)
states how the default output limit covers both. A sink that needs a raw
intermediate and has none is an argument error before any mutation. The
transcode reads the file in chunk-sized buffers and spares only the reads of
the ranges the file's own [written-range
record](design-holes.md#the-written-range-record) proves zero.

### Transcode

The transcode is one sequential pass over a disk object into a sequential
sink. The container encoder and the gzip encoder are each a sequential sink
wrapping the next; the innermost sink is the staged output file (positional,
written at increasing offsets) or a stream descriptor. The pass reads the disk
in chunks of the container's chunk size (or 1 MiB with no container) and, for
each chunk, decides by inspecting its bytes whether it is all zero.

Zero decisions depend only on the bytes: an all-zero chunk becomes a DMG
zero-fill entry, a simg `DONT_CARE` chunk, an unwritten range of a VHD or a
raw copy, or a run of zeros for gzip. A zero range the disk object reports,
under the [hole design](design-holes.md#the-zero-range-operation), may only
spare the read of a chunk lying wholly inside it; it cannot change an output
byte. The raw intermediate answers from the written-range record its
emission built, an assembled disk answers its zero gaps and its inputs'
answers, and the host is never asked about holes in a file fsimg wrote.
Consecutive zero chunks merge into one entry where the container allows
it, so a sparse 128 TiB disk yields a few table entries, not 128 million.

Each container's sequential order is fixed by the format and listed in its
profile: DMG writes the data fork, then the XML plist, then the koly trailer;
VHD writes the disk then its footer; simg writes its header, then chunks.
simg's header carries the chunk count, which the zero decisions determine, so
simg runs the inspection pass twice: the first counts chunks and ends by
writing the header, the second emits the chunks and fails as a changed
source if it emits another count, as an input read twice by wrap or compose
can. Both passes spare only the reads of reported zero ranges. This keeps
positional and stream sinks identical and the bytes deterministic.

Chunk records that a container must emit after its data (the DMG block
table) go through a metadata scratch record stream as they are decided, and
are replayed in order when the table is written. No table is held in memory.

### Streams

`-f -` on create writes the transcode's output, or for a stack without a
container or compression a sequential copy of the raw intermediate, to a
non-seekable sink. The sink is a descriptor or a callback; the CLI carries
the bytes from the worker to standard output over the same bounded raw data
path as `x -O`. There is no atomic publication on a stream: a failure after
any byte has been written is reported as incomplete output
(`FSIMG_OUTPUT_INCOMPLETE`) through the error record and the exit status, and
the consumer must discard what it received. A failure before the first byte
reports absent output.

### Create then verify

A create-then-verify operation (CLI `cW`) opens the staged file as an image,
verifies every layer and then the filesystem with the ordinary verification
scopes, and only then publishes it. A compressed staged file is opened as
gzip, decoded with its member checks into a [spool](#stream-input-and-the-spool)
under the spool limit, and verified from there, so it needs scratch. It applies to file output only: a stream
has no complete artefact to open before its bytes are gone, and the
[CLI design](design-cli.md#creation) refuses the combination. A verification
failure discards the staged files and publishes nothing; the error names the
failing layer and states that nothing was published.

### Failure

Every failure from the first mutation of the staged disk on marks the output
incomplete; the coordinator removes staged files and publishes nothing. The
raw intermediate has no name and needs no cleanup; the staged output is
removed by the existing cleanup. Cancellation is polled at every chunk of the
transcode and at every scheme table write. An external input observed
shorter than declared at a hole lookup is a changed input, and the chunk
holding such an input's last byte is always read, so an input still short
there fails at that read
([hole design](design-holes.md#external-files-the-hosts-report)). A partial container, a missing
trailer, a plist without its koly, or a disk with primary but no backup GPT
is never published.

## Wrap, unwrap and compose

Three image-level operations reuse the same machinery without a filesystem
writer: `fsimg_image_wrap`, `fsimg_image_unwrap` and `fsimg_image_compose`.
They are library operations with no command-line form.

* **Wrap** takes an existing raw volume or disk and a stack whose filesystem
  slot is empty or names the volume's format, and produces the stacked image.
  With a scheme in the stack the input is a volume placed in one partition;
  its default partition type comes from probing the volume (unambiguous or
  fail) unless `type=` is given. Without a scheme the input is the disk.
* **Unwrap** takes an opened image and a Disk, partition or Range selector and
  writes the selected bytes raw.
* **Compose** takes an ordered list of volume images, each opened as an image
  and selected to a volume, with a name, a type and a partition GUID (under
  the [identifier rule](#identifiers)) per partition, and a stack
  with an empty filesystem slot and a scheme. Each volume becomes one
  partition, placed in order with the same alignment as a written disk; MBR
  accepts at most four. Partition types default from probing each volume.
  Every partition setting comes from its part, so a compose whose layer
  options carry a partition key (`gpt:type`, `gpt:name`, `gpt:guid`,
  `mbr:type`, `mbr:active`) fails; the disk keys apply to the whole disk.

Wrap and compose do not stage a raw disk. The disk is **assembled**: a
read-only positional object over ordered regions, each a scheme table buffer
held in charged memory (at most the two GPT header and entry copies plus the
MBR, under 64 KiB), a borrowed slice of an input, or a zero gap. The transcode
and the sequential copy read the assembled disk exactly as they read a raw
intermediate, so the output path has one input type. The assembled disk
answers the transcode's zero-range question region by region, under the
[hole design](design-holes.md#what-each-object-answers): a zero gap is zero,
a table never is, and an input slice answers as its source does, a
container adapter from its chunk index, a spooled image from its spool's
record, a caller's object through its callback, and a user's file from the
host's hole report after one sync, never for the chunk holding its last
byte, which is always read, or nothing when the sync fails, so that file is
read in full. An input slice may itself sit above a
container adapter; that is the one place an adapter sits above another, and
the depth stops there. Compose with a container therefore reads every input
once, sparing the ranges its inputs prove zero, and writes the output once.
Unwrap is the sequential copy with no encoder. Inputs are checked against
the output for backing-identity aliases before any mutation, like writer
sources.

All three accept the write options for mode (reproducible flag, epoch,
durability) and the layer options; filesystem settings must hold their
defaults because no filesystem is being written, the same rule as for any
absent layer.

## Settings

| Key | Type | Default | Validation |
| --- | --- | --- | --- |
| `gpt:type` | type name, or GUID text | Derived from the filesystem: `basic` for FAT and exFAT, `qnx6` for QNX6; none for QNX4, which must name its type | Name from the [type table](#partition-types) or RFC 4122 text; not nil, which marks an unused entry |
| `gpt:name` | UTF-8 text | The type's conventional name from the [type table](#partition-types); an empty value is the default | At most 36 UTF-16LE units after conversion; validated UTF-8 |
| `gpt:disk-guid`, `gpt:guid` | GUID text | See [identifiers](#identifiers) | RFC 4122 text; not nil, which the library reads as unset |
| `gpt:disk` | bytes | Derived from the volume | Multiple of the sector size and the container granularity; large enough for the volume and tables |
| `gpt:sector` | bytes | 512 | 512 or 4096; the LBA size of the tables. On read it resolves a disk carrying a header at both offsets and requires `gpt` in the forced stack |
| `mbr:type` | type name, or `0x` byte | Derived: `fat12` 0x01, `fat16` 0x06, `fat32` 0x0C, `exfat` 0x07, `qnx6` 0xB2, `qnx4` 0x4D | Nonzero byte; not 0x05, 0x0F or 0x85 (extended containers) and not 0xEE (GPT protective) |
| `mbr:active` | `yes`, `no` | `no` | Sets the 0x80 status |
| `mbr:id` | `0x` 32-bit | See [identifiers](#identifiers) | Any value |
| `mbr:disk` | bytes | Derived | As `gpt:disk`; at most 2 TiB, the 2^32 sectors an entry can address |
| `dmg:compress` | `zlib`, `none` | `zlib` | `none` writes raw data chunks |
| `dmg:level` | integer | 6 | 1 to 9; the zlib level of UDZO chunks |
| `dmg:chunk` | bytes | 1 MiB | Multiple of 512 from 512 to 16 MiB, the [read-side maximum](#chunk-index-and-cache) |
| `dmg:uuid` | GUID text | See [identifiers](#identifiers) | RFC 4122 text; the segment ID |
| `vhd:align` | bytes | 512 | Power of two from 512 to 1 GiB; the disk-length granularity |
| `vhd:uuid` | GUID text | See [identifiers](#identifiers) | RFC 4122 text |
| `simg:block` | bytes | 4096 | Power of two from 1 KiB to 1 MiB, so the 1 MiB inspection unit is whole blocks |
| `simg:crc` | `yes`, `no` | `no` | Appends a CRC32 chunk |
| `gzip:level` | integer | 6 | 1 to 9; only when gzip is selected (`-z`, a `.gz` name under `-a`, or the library's write options naming `gzip`) |

Values use the CLI's existing byte-quantity, integer, UUID and boolean
syntax. A key whose layer is absent from the stack fails as an unknown
setting. Duplicate keys are syntax errors.

The zlib levels default to 6, zlib's own default and `gzip(1)`'s: the last
level before compression time grows steeply for little gain. What `hdiutil`
uses for UDZO is to verify by comparing its output size against fsimg's at
each level; a difference changes nothing here, since any level yields a
valid image, but is recorded so size comparisons with `hdiutil` output are
read correctly.

### Partition types

One table serves `type=` on write, the Type selector on read, compose and
`--info`:

| Name | GPT type GUID | MBR byte | Default GPT name | Meaning |
| --- | --- | --- | --- | --- |
| `basic` | `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` | 0x07 | `Microsoft basic data` | Microsoft basic data (FAT, exFAT, NTFS) |
| `esp` | `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` | 0xEF | `EFI system partition` | EFI system partition |
| `fat12`, `fat16`, `fat32` | as `basic` | 0x01, 0x06, 0x0C | as `basic` | FAT by MBR convention; FAT16 may also carry 0x04 (below 32 MiB) or 0x0E (LBA) and FAT32 0x0B, accepted on read |
| `exfat` | as `basic` | 0x07 | as `basic` | exFAT |
| `linux` | `0FC63DAF-8483-4772-8E79-3D69D8477DE4` | 0x83 | `Linux filesystem` | Linux filesystem data |
| `apfs` | `7C3457EF-0000-11AA-AA11-00306543ECAC` | none | `Apple APFS` | APFS container |
| `hfs` | `48465300-0000-11AA-AA11-00306543ECAC` | 0xAF | `Apple HFS/HFS+` | Apple HFS+ |
| `qnx6` | `CEF5A9AD-73BC-4601-89F3-CDEEEEE321A1` | 0xB2 | `QNX6 Power-Safe` | QNX6 Power-Safe; 0xB1 and 0xB3 accepted on read |
| `qnx4` | none | 0x4D | none | QNX4 POSIX volume; 0x4E and 0x4F, the secondary and tertiary volumes, accepted on read |

The default GPT names are the ones `sgdisk` assigns, so a disk fsimg writes
lists like one it made. A GPT type that is not in the table is reported as
its GUID text; an MBR type as `0x` and two hex digits. The QNX values come
from QNX's own documentation: the QNX6 GUID is the one the `io-blk.so` and
`diskimage` references assign to Power-Safe partitions, also listed by
gptfdisk (`0xb300`, "QNX6 Power-Safe") and util-linux ("QNX6 file system");
0xB1, 0xB2 and 0xB3 are the Power-Safe types and 0x4D, 0x4E and 0x4F the QNX4
types of the QNX system architecture guide's partition table and the QNX
`fdisk` reference. Other operating systems' lists also give 0xB1 and 0xB3 to
HP volume expansion, which a type selection cannot tell apart; fsimg writes
0xB2, the type QNX's own `fdisk` assigns, which no other list claims. A
type with no MBR byte cannot be requested on an MBR disk. A filesystem with no
GPT type has no `gpt:type` default: QNX4 predates GPT and no published list
registers a GUID for it, so `qnx4/gpt` without `gpt:type=` is `FSIMG_EINVAL`
naming the key rather than an identity no other tool recognises.

## Resource bounds

### Chunk index and cache

Every container is, on read, a **chunk index**: records that tile the disk,
each naming a disk range and how to obtain its bytes. One record is forty
bytes, little-endian: disk offset at 0, length at 8, file offset at 16 and
encoded length at 24, each 64-bit; a 32-bit kind at 32 (raw bytes at the
file offset, zero, a repeated fill value, or compressed bytes); and a 32-bit
argument at 36 holding the fill value for a fill record, the codec number
for a compressed record, and zero otherwise. DMG and simg both fill it; a
fixed VHD is the degenerate case, one raw record covering the file before
the footer, so its disk object is simply a slice. The index owns:

* append of validated records to a sealed scratch record stream, charged to
  scratch, then one external sort by disk offset and one sequential tiling
  pass that rejects overlap, gaps, a first record not at zero and a total
  other than the declared disk length. The chunk count needs no separate
  limit: each chunk costs at least 12 (simg) or 54 (DMG, base64) bytes of the
  file, so the record stream is bounded by the file length, and 128 million
  records cost 5 GiB of scratch, inside the scratch budget;
* checked decoding of every record read back, as the extent index does,
  because scratch is not trusted;
* lookup of the record containing a disk offset through a cached last record
  and otherwise binary search over the sealed store;
* release with the image.

The container adapter resolves every read through the index: raw records read
the file at the translated offset; zero and fill records synthesise bytes;
compressed records go through the **chunk cache**, a least-recently-used
cache of decompressed chunks charged to the memory budget. A decompressed
chunk is at most 16 MiB, and a declared chunk larger than that is
unsupported, not corrupt; `hdiutil` writes chunks of 1 MiB, far inside the
limit. The cache holds four chunks of the image's largest
declared decompressed chunk, so 4 MiB for `hdiutil`'s 1 MiB chunks and
64 MiB at the limit. Each adapter has its own cache, so composing several
compressed inputs needs a budget that holds every cache; a budget that
cannot fails the open as a memory limit naming the chunk cache rather than
shrinking it.

Decoding work is bounded on both sides of the codec. A compressed run is at
most zlib's `compressBound()` of its decompressed length, so a run cannot
pad its input with gigabytes of empty stored blocks; a longer run is
`FSIMG_ECORRUPT`. Compressed bytes read and decoded bytes produced are both
charged to the work counter, so `-l work=` and the deadline bound
decompression time. Without a work limit, an adapter still stops at an
algorithmic bound: its decoded bytes may not exceed 64 times the summed
decoded length of its compressed records plus 1 GiB, beyond which the read
fails as a work limit naming the chunk cache. The bound counts only what
compressed records declare, which the image pays for in compressed bytes,
so a disk declared vast through zero runs does not raise it. That bound is
policy, to calibrate against fragmented real images.
The amplification an adversarial image can force is therefore bounded: a
read costs at most one chunk decode when it misses the cache; sequential
reads and any access pattern confined to four chunks decode each chunk
once; and a pattern that alternates between more chunks than the cache
holds pays one decode per read until the work bound stops it.
Decompression writes into a buffer sized exactly from the
declared sector count, and a chunk that produces fewer or more bytes than
declared, or whose compressed bytes do not end exactly at its declared
compressed length, is corrupt. The cache changes no byte: cache size, like
page-cache size, is invisible in results.

### Plist parser

The DMG reader parses an untrusted XML property list. It implements only what
UDIF uses, as a streaming parser over bounded windows of the file:

* Elements: `plist`, `dict`, `key`, `array`, `string`, `data`, `integer`,
  `true`, `false`. The prolog (`<?xml ...?>`, `<!DOCTYPE ...>`) and comments
  are skipped without interpretation; attributes are skipped; CDATA sections,
  processing instructions after the prolog, and any other element are
  unsupported.
* Entities: only the five predefined (`&lt;`, `&gt;`, `&amp;`, `&quot;`,
  `&apos;`). Numeric character references and a DOCTYPE internal subset
  are unsupported; with no subset to declare one, a reference to any other
  entity is undeclared, and malformed.
* Limits: nesting depth 32; element name 16 bytes; key, string and integer
  text 4 KiB; the plist length is bounded by the koly's XML length, itself
  bounded by the file; a `data` element is decoded from base64 in bounded
  pieces straight into chunk-index records and never materialised, skipping
  the ASCII whitespace (`hdiutil` wraps the text with tabs and newlines)
  and rejecting any other byte outside the alphabet.
* Unknown keys at any level are skipped as a bounded subtree under the same
  depth limit. The parser charges work per element and polls cancellation.
* A construct beyond the subset or a limit crossed is
  `FSIMG_EUNSUPPORTED`, naming the construct; malformed XML is
  `FSIMG_ECORRUPT`.

### Tables and output

The spool limit is stated under [stream input](#budget) and the raw
intermediate's share of the output limit under
[staging](#staging-and-the-raw-intermediate). Scheme tables are bounded by
their format: at most 1024 GPT entries of 128 bytes are accepted on read
(128 KiB per copy), written tables are the standard 128 entries. Nothing in a
layer grows with disk length in memory.

## Identifiers

Every scheme and container with an identity field takes it from a setting
used verbatim, as the volume serial and the QNX6 UUID are: `gpt:disk-guid`
(the disk GUID), `gpt:guid` (the partition's unique GUID), `mbr:id` (the
32-bit disk identifier), `vhd:uuid` and `dmg:uuid` (the segment ID of the
single segment). The library has no fixed default for them: a nil GUID is
reserved and any constant collides, so outside reproducible mode an unset
identifier for a layer in the stack is `FSIMG_EINVAL` naming the key, and no
library caller ships a constant by accident. The CLI supplies random
version 4 values without `-R`, exactly as it supplies the random volume
serial.

In compose every part carries its own partition GUID under the same rule:
required outside reproducible mode, with the error naming the partition,
and derived from the partition index under it. Compose checks that every
partition GUID differs from the others and from the disk GUID, and accepts
at most the 128 entries a written table holds; a violation is
`FSIMG_EINVAL` naming both partitions or the excess count.

Under reproducible mode an unset identifier is derived from the epoch and a
role, as a 128-bit GUID value: its 16 bytes in RFC 4122 text order are the
epoch as a little-endian 64-bit value, one role byte (1 disk, 2 partition,
3 VHD, 4 DMG segment), one byte holding the partition's 1-based slot
(zero for every other role) and six zero bytes; then
the version nibble replaces the high nibble of byte 6 with 4 and the
variant bits replace the top two bits of byte 8 with `10`, so tools accept
the GUID as well-formed. That value is then encoded as each format stores
GUIDs: GPT in its mixed byte order, VHD and DMG in text byte order. The MBR
disk identifier is the low 32 bits of the epoch. Two reproducible images at
the same epoch therefore share identifiers. Two such images attached to one
host collide: Linux `by-partuuid` links name only one of them, and Windows
takes a disk with a duplicate signature offline. The plan and `i` print every
derived value, and a build that attaches such images side by side sets them
explicitly. The derivation stays the default because a reproducible build
must not fail for an option its non-reproducible twin never needed, and a
digest of the volume bytes would cost one more sequential read of the staged
volume for a collision that arises only when same-epoch images meet on one
host.

## Reproducibility

Under reproducible mode layer bytes are a function of the inputs and options:

* Identifiers follow the [identifier rule](#identifiers).
* Timestamps are the epoch, clamped to each field's range: the VHD footer
  timestamp counts seconds since 2000-01-01 UTC, so the default 1980 epoch
  and any earlier one encode as zero; the gzip header time is an unsigned
  32-bit count of seconds since 1970, and an epoch below one or above
  2^32 - 1 encodes as zero, which RFC 1952 defines as "no time stamp
  available", the honest value for a time the field cannot hold. Outside
  reproducible mode both carry the host clock under the same clamps.
* The DMG plist text is produced by one fixed template with fixed key order
  and whitespace; the chunk size is fixed by the setting; chunk boundaries
  and zero decisions depend only on the disk bytes.
* GPT and MBR tables contain nothing host-dependent.

Compressed bytes are reproducible only for a given zlib implementation and
version: zlib-ng in compatibility mode, which some distributions ship as
their zlib, produces different, equally valid, streams. Capability output
and the plan report the zlib version string so a reproducibility claim can
name it. `dmg:compress=none`, `simg` and `vhd` output is reproducible without
that qualification.

## Verification scope

Layer verification runs before the filesystem's and reports its own scope:

| Layer | Checks |
| --- | --- |
| gzip | Each member's CRC32 and modular length, at spool time; trailing bytes |
| DMG | koly fields and ranges, the data-fork CRC32 by one sequential pass over the data fork where the koly declares it, the master CRC32 over the tables' checksum values where the koly declares it, each block table's CRC32 over its decoded bytes under payload read-through, chunk tiling and bounds, every chunk decoded once under read-through |
| VHD | Footer checksum and the constant fields (format version, data offset, saved state); the cookie, type 2 and the size tie are identity and already hold at open |
| simg | Header and chunk-header consistency, block totals, CRC32 chunk where present under read-through |
| GPT | Protective MBR, primary and backup header CRC32 and entry-array CRC32, primary and backup consistency (disk GUID, usable range, entry arrays byte-identical), partitions within the usable range and non-overlapping |
| MBR | Signature, entry field sanity, partitions within the disk and non-overlapping |

Then the selected volume is verified by `fsimg_verify()` as for a raw image.
Partitions not selected are not verified, and the report says so. Ordinary
open already performs every table check: header, trailer and entry
validation, chunk tiling and bounds, partition bounds and overlap, so a
damaged table never yields a volume. Verification adds the passes that read
beyond the tables: the DMG data-fork and master CRC32s, and under
read-through the per-table, simg and member checksums with every chunk
decoded.

## Dependencies and capabilities

zlib is a required dependency, linked from the system as the
[architecture](architecture.md#dependencies-and-implementation-organization)
permits: gzip and DMG zlib chunks (UDZO) are always available, and configure
fails clearly without it. Other DMG chunk codecs (ADC, bzip2, lzfse, lzma)
are unsupported chunk compression and fail with an explicit error naming the
type; they are never silently treated as zeros. zstd and xz are reserved
compression names for later profiles.

Capability enumeration reports each layer with its kind, read and write
flags, settings, codecs and the zlib version string, through the same
immutable registry mechanism as formats. Runtime sandbox reporting is
unchanged.

## Security

Layers add attack surface before any filesystem byte is read. The rules:

* **Tables are untrusted.** Every koly, footer, header, entry and chunk
  record is decoded explicitly from bytes, with checked arithmetic, and every
  offset and length is validated against the enclosing object (file, data
  fork, disk) before use. A declared size never sizes a buffer: buffers come
  from settings and profile limits, and the declared value is checked against
  them. Partition slices are checked slices; a partition cannot reach outside
  the disk, and a chunk cannot reach outside the data fork.
* **Decompression is bounded twice.** A chunk decompresses into a buffer
  sized by its declared sector count, capped by the profile maximum, and must
  fill it exactly; the compressed input is bounded by its declared length.
  gzip decompresses into the spool under the spool limit, reserved before
  each step; the decoder's state is charged to memory. An inflate that
  produces more than declared stops at the buffer end and is corrupt.
* **The layers add no name authority.** The spool and the raw intermediate
  are [handed-over files](#handed-over-files): the worker receives open
  descriptors to unlinked files, and its authority over names stays what
  the [security design](design-security.md#coordinator-and-worker) maps out
  for the staging directory and the scratch tree. The gzip decoder and
  every table parser run inside the confined worker, after acknowledgement,
  on bytes the coordinator pumps or passes without reading.
  A compromised worker can fill its spool to the host's limits, as it already
  can with scratch; a deployment needing a hard ceiling uses host quotas, as
  the [security design](design-security.md#other-boundaries) states. The
  coordinator's publication check still sees exactly one staged `image`.
* **Zero bytes are written, not assumed.** Transcode output zeros come from
  explicit zero chunks or from the unwritten ranges of a fresh file, which
  its [written-range record](design-holes.md#the-written-range-record)
  proves, never from a reused file's old contents or a host's report of its
  holes. Alignment padding, reserved table fields and the MBR code area are
  emitted as zeros.
* **Forcing selects, never relaxes.** A forced layer runs full validation;
  the range selector bypasses the scheme but not the slice checks.
* **No path from a name.** Partition names and DMG `Name` strings are
  displayed through the CLI's escaping and never used to open anything.

## Layer profiles

Each profile states the on-disk facts fsimg relies on, what it writes, what it
checks on read, and the independent oracle that gates the claim. Field layouts
below are recorded from the formats' public documentation and are verified
against oracle output before the codec is accepted; a mismatch changes the
profile, never the oracle.

### GPT

Reference: UEFI Specification, "GUID Partition Table (GPT) Disk Layout".
Oracles: `sgdisk`/`gdisk` (`-v` verification, `-p` listing, disks created by
`sgdisk`), `qemu-img`, and `hdiutil` for DMG-wrapped disks.

Layout, with `S` the sector size (512 or 4096) and `L` the last LBA:

| LBA | Content |
| --- | --- |
| 0 | Protective MBR: zero code area, disk identifier zero, one entry of type 0xEE, status 0, CHS start 0x000200, CHS end 0xFFFFFF, first LBA 1, size `min(L, 0xFFFFFFFF)`, signature 0x55AA |
| 1 | Primary header |
| 2 .. | Primary entry array, 128 entries of 128 bytes (16 KiB, `32` LBAs at 512 or `4` at 4096) |
| first usable .. last usable | Partitions; the written volume starts at the first 1 MiB multiple at or above the first usable LBA |
| `L - entry LBAs` .. `L - 1` | Backup entry array |
| `L` | Backup header |

Header (92 bytes, little-endian): signature `EFI PART`, revision 0x00010000,
header size 92, header CRC32 (computed with the field zero), reserved zero,
this LBA, alternate LBA, first usable LBA, last usable LBA, disk GUID, entry
array LBA, entry count, entry size 128, entry array CRC32; the rest of the
sector is zero. Entry: type GUID, unique GUID, first LBA, last LBA
(inclusive), attributes (written zero), name as 36 UTF-16LE units, zero
padded.

A GUID `AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE` is stored in 16 bytes as the
three leading groups little-endian and the rest in text order: bytes 0 to 3
hold `AAAAAAAA` with its least significant byte first, bytes 4 and 5 `BBBB`
and bytes 6 and 7 `CCCC` likewise, bytes 8 and 9 `DDDD` and bytes 10 to 15
`EEEEEEEEEEEE` in the order written. The RFC 4122 version nibble, the first
hex digit of `CCCC`, is therefore the high nibble of on-disk byte 7, and the
variant bits, the top bits of `DDDD`, are the top bits of on-disk byte 8.
The [identifier derivation](#identifiers) sets them there. Settings and
reports use the text form.

Writing: one used entry, the rest zero; both header copies and both entry
arrays; `gpt:sector` chooses `S`. The first usable LBA is the LBA after the
primary entry array, 34 at 512 and 6 at 4096, and the last usable LBA is the
LBA before the backup entry array, `L - 33` at 512 and `L - 5` at 4096, the
values `sgdisk` writes for a 128-entry table. A 4Kn table inside a DMG, whose
sectors are always 512 bytes, is legal to write but is attached by macOS
only if `hdiutil` accepts it, an evidence gate.

Reading: the probe claims on identity: a protective MBR entry of type 0xEE
at LBA 0, or the `EFI PART` signature at offset 512 or 4096. The signature's
offset fixes `S`; a signature at both offsets is ambiguous and needs `gpt` in
the forced stack with `gpt:sector`; a protective MBR with the signature at
neither offset is a claim whose header is missing, so it is corrupt rather
than read as something else. Validation then requires: header CRC32, this
LBA 1, alternate LBA equal to the disk's last LBA, entry size 128, entry
count from 1 to 1024, entry array within the disk and before the first usable
LBA, entry array CRC32, first usable not after last usable and last usable
before the backup array, the backup header at the alternate LBA with its own
CRC32, alternate pointing back to 1, the same disk GUID and usable range, and
a byte-identical entry array. Used entries (nonzero type) need first not
after last and both within the usable range; the at most 1024 entries are
sorted in memory by first LBA and adjacent pairs checked for overlap. Any
failure is `FSIMG_ECORRUPT`; there is no fallback to the backup copy alone,
because a disk whose copies disagree is reported, not repaired. A truncated
image whose backup is missing is therefore corrupt, which is what it is.
Truncated images are common, but a silent fallback would hide the damage; a
forced degraded open that reports it in the layer report is the extension if
demand appears.

### MBR

Reference: the classic master boot record layout as documented by Microsoft
and the UEFI specification's "Legacy Master Boot Record". Oracles: `sfdisk`
and `fdisk` listings, `qemu-img`, `mtools` reading a FAT partition through an
MBR image.

Layout: 440 bytes of code area (written zero), a 32-bit little-endian disk
identifier at 440, two zero bytes, four 16-byte entries at 446, signature
0x55AA at 510. Entry: status (0x00 or 0x80), CHS start (3 bytes), type, CHS
end (3 bytes), first LBA (32-bit), sector count (32-bit). Sectors are 512
bytes. An entry addresses sectors 0 to 2^32 - 1: its first LBA plus its
count, computed in 64 bits, is at most 2^32, so a disk of up to 2^32 sectors
(2 TiB) is fully addressable and a partition ending beyond that cannot be
expressed; `mbr:disk` caps the disk there.

Writing: one entry in slot 1, the others zero. CHS fields encode the LBA
under the 255-head, 63-sector convention and saturate at cylinder 1023, head
254, sector 63, as `fdisk` writes them, so firmware and tools that still read
CHS see consistent values.

Reading: the probe claims on identity, which for an MBR is the signature and
the shape of the entry area, since a FAT boot sector carries the same
signature: every entry all zero or with a legal status (0x00 or 0x80), a
nonzero type and a nonzero count, at least one entry used, and no entry of
type 0xEE, which belongs to GPT's protective MBR, so a hybrid MBR is a GPT
disk. Validation then requires each used entry to start after the MBR's own
sector (first LBA at least 1), first LBA plus count within the disk, and no
overlap between used entries; a failure is `FSIMG_ECORRUPT`. Extended
partitions (types 0x05, 0x0F and 0x85) are listed with their type and are
unsupported as a selection: logical partitions inside them need their own
profile. A sector whose bytes also draw a filesystem claim is ambiguous, as
[detection](#detection-and-forcing) states.

### DMG (UDIF)

Reference: no public specification; the layout below follows the widely
documented reverse-engineered description used by `dmg2img`, `libdmg-hfsplus`
and QEMU's `dmg` block driver, and is verified against `hdiutil` output.
Oracles: `hdiutil imageinfo`, `hdiutil verify`, `hdiutil attach` (the
evidence that macOS mounts the volume), `hdiutil create`/`convert` as the
producer of images fsimg must read, `qemu-img info`/`convert`, and `7z l`.

File order: data fork of chunks, XML plist, 512-byte koly trailer at the end.
All integers are big-endian. The trailer:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Signature `koly` |
| 4 | 4 | Version 4 |
| 8 | 4 | Header size 512 |
| 12 | 4 | Flags (1, flattened) |
| 16 | 8 | Running data fork offset (0) |
| 24 | 8 | Data fork offset |
| 32 | 8 | Data fork length |
| 40 | 8 | Resource fork offset (0) |
| 48 | 8 | Resource fork length (0) |
| 56 | 4 | Segment number (1; `hdiutil create` writes 0) |
| 60 | 4 | Segment count (1; `hdiutil create` writes 0) |
| 64 | 16 | Segment ID |
| 80 | 136 | Data fork checksum: type (2 = CRC32, 0 = none), bit size (32), 32 x 32-bit values |
| 216 | 8 | XML offset |
| 224 | 8 | XML length |
| 232 | 120 | Reserved |
| 352 | 136 | Master checksum, same layout |
| 488 | 4 | Image variant (2; `hdiutil` writes 1 beside per-region tables) |
| 492 | 8 | Sector count |
| 500 | 12 | Reserved |

The plist is `<plist><dict>` with a `resource-fork` dict holding a `blkx`
array; each element is a dict with `Attributes` (string `0x0050`), `CFName`
and `Name` (strings), `ID` (string, decimal) and `Data` (base64 of a `mish`
table). The `mish` table, big-endian like the trailer, follows the layout
`libdmg-hfsplus` and `dmg2img` document. The relations the reader relies on
are confirmed against `hdiutil` output: every image it writes has data
offset 0, tables starting at nonzero sectors with run sectors counted from
the table's first sector, and table and master checksums as stated below.
The values in the "Written" column are confirmed against `hdiutil` output
and a QEMU read of an fsimg image before the writer is accepted:

| Offset | Size | Field | Written |
| --- | --- | --- | --- |
| 0 | 4 | Signature `mish` | |
| 4 | 4 | Version 1 | |
| 8 | 8 | First sector of the table within the disk | 0 |
| 16 | 8 | Sector count of the table | The disk's |
| 24 | 8 | Data offset: the base, relative to the data fork start, that every run's compressed offset adds to | 0 |
| 32 | 4 | Buffers needed | The chunk's sector count plus 8, as `hdiutil` writes (0x808 for 2048-sector chunks) |
| 36 | 4 | Block descriptor: the partition number the table describes, not a count | 0xFFFFFFFE, the whole disk, as `hdiutil convert` writes for a single table |
| 40 | 24 | Reserved | Zero |
| 64 | 136 | Checksum of the table's decoded bytes, every run but Ignore runs: type (2 = CRC32), bit size (32), 32 x 32-bit values, the first used | CRC32 |
| 200 | 4 | Run count, terminator included | |
| 204 | 40 each | Runs: type (32-bit), comment (32-bit), sector number relative to the table's first sector, sector count, compressed offset, compressed length (each 64-bit) | |

Sectors are 512 bytes. The trailer's data-fork checksum is the CRC32 of the
data fork bytes; the master checksum is the CRC32 over the concatenated
big-endian 32-bit per-table checksum values in table order, zero values
included. A table's checksum covers the decoded bytes of its runs in order,
zero runs as zeros, and skips Ignore runs, so a table of free space alone
has the empty CRC, zero. Run types:

| Type | Meaning | Read | Write |
| --- | --- | --- | --- |
| 0x00000000 | Zero fill | Zero | All-zero chunks, merged across neighbours |
| 0x00000001 | Raw | Copy | Data chunks under `compress=none` |
| 0x00000002 | Ignore (free) | Zero | Never |
| 0x80000004 | ADC | Unsupported | Never |
| 0x80000005 | zlib (UDZO) | Inflate | Data chunks under `compress=zlib` |
| 0x80000006 | bzip2 | Unsupported | Never |
| 0x80000007 | lzfse | Unsupported | Never |
| 0x80000008 | lzma | Unsupported | Never |
| Any other | Unknown | Unsupported, naming the type | Never |
| 0x7FFFFFFE | Comment | Skipped; zero sectors | Never |
| 0xFFFFFFFF | Terminator | Ends a table; zero sectors | Last entry |

Reading: the probe claims on identity, tied to the file length it receives:
`koly`, version 4 and header size 512 in the last 512 bytes, and the XML
range ending exactly at the trailer (XML offset plus XML length equal to the
file length minus 512). Every `hdiutil` image ends its data fork exactly at
the XML offset and its XML exactly at the trailer. Validation then requires:
the data fork ending at or before the XML offset; a single segment, segment
number and count both 1 (`hdiutil convert`) or both 0 (`hdiutil create`),
any other pair being a multi-segment image, which is unsupported; sector
count times 512 as the disk length; checksum types 0 or 2, a CRC32 of 32
bits. The plist is parsed as
the [bounded subset](#plist-parser) states, and every table and run is
appended to the chunk index: a table's first sector and sector count within
the disk, each run's sector range within its table, compressed offset plus
length within the data fork, raw runs with compressed length equal to sector
count times 512, zero and ignore runs with compressed length zero, compressed
runs with a nonzero compressed length and a decompressed length of at most
16 MiB, and a nonzero sector count for every run other than a comment or the
terminator, which carry zero by definition. A table holds exactly its
declared run count and ends with its one terminator. Run sector counts are
summed per table and must equal the table's count. The index's tiling pass
then proves that the tables and runs together cover exactly the declared
sector count with no overlap; an image with uncovered sectors is corrupt
rather than silently zero. Every `hdiutil` image examined (UDRO, UDZO, UDRW
converted to both, zero-filled, with no partition map, GPT and MBR) tiles
its disk exactly, one table per partition-map region with its free space as
Ignore runs, so the strict rule costs no real image. `hdiutil` writes UDZO
chunks of 2048 sectors and stores a chunk raw when compression does not
shrink it. The `plst` resource and other resource-fork keys are skipped.

Writing: one block table covering the whole disk, `ID` 0, `Name` and
`CFName` `whole disk (fsimg : 0)`, runs of `dmg:chunk` bytes with a shorter
last run, zero runs merged, a terminator entry, data runs as UDZO at
`dmg:level` under the default `dmg:compress=zlib` and raw under `none`, the
table's CRC32 over the decompressed disk bytes, and the data-fork CRC32 and
master CRC32 in the trailer. Zero runs carry the current data fork end as
their compressed offset, as `hdiutil` writes them. `hdiutil` verifies,
attaches and mounts such an image with a GPT, an MBR or a bare FAT volume
and no partition map, as an ordinary user, under image variant 2; it refuses
to attach variant 1 when the single table spans a partitioned disk, so
variant 2 is written for every stack. `qemu-img convert` expands each of
them to the raw disk byte for byte, including a zero run of more than
64 MiB.

### Fixed VHD

Reference: Microsoft, "Virtual Hard Disk Image Format Specification",
version 1.0. Oracles: `qemu-img info`/`check`/`convert`, and a hypervisor
attach where available. `qemu-img` must be told the format (`-f vpc`):
probing reads a fixed VHD as a raw file. `qemu-img convert -O vpc -o
subformat=fixed` pads the disk to its CHS geometry unless `force_size=on`
is given, so a GPT converted without it no longer ends at the disk's last
sector and fails the GPT checks, as it should.

Layout: the raw disk followed by a 512-byte big-endian footer: cookie
`conectix`, features 0x00000002, format version 0x00010000, data offset
0xFFFFFFFFFFFFFFFF, timestamp (seconds since 2000-01-01 00:00:00 UTC, as
[reproducibility](#reproducibility) states), creator application, creator
version, creator host OS, original size, current size, disk geometry
(cylinders 16-bit, heads 8-bit, sectors per track 8-bit) computed by the
specification's CHS algorithm, disk type 2 (fixed), checksum (one's
complement of the byte sum of the footer with the checksum field zero),
unique identifier (16 bytes, `vhd:uuid` in text byte order), saved state 0,
427 reserved zero bytes.

Writing: the disk, then the footer. The creator application is `fsim`, the
creator version fsimg's major and minor release numbers as two 16-bit
fields, and the creator host OS `Wi2k`, the value Windows-created images
carry and the one every reader accepts; `Mac ` is the only alternative the
specification defines and some tools reject it. Original size equals current
size, as for any image not resized since creation. `vhd:align` pads the disk
length to a multiple of the alignment first; 1 MiB is what Azure requires of
a VHD's virtual size, so `vhd:align=1M` is the documented recipe for that
target.

Reading: identity is the cookie in the last 512 bytes, the disk type, and
for type 2 the tie to the file length, a current size equal to the file
length minus 512. A dynamic or differencing image (type 3 or 4) has no size
tie, so its identity is the footer copy at offset 0 being byte-identical to
the trailer and its data offset pointing inside the file; a bare volume
whose last sector merely ends a stored dynamic VHD therefore draws no claim.
Validation then requires a correct checksum, format version
0x00010000, data offset all ones, saved state 0 and current size a multiple
of 512, each failure `FSIMG_ECORRUPT`; types 3 and 4 (dynamic and
differencing) are `FSIMG_EUNSUPPORTED`. The disk is the file before the
footer.

### Android sparse image (simg)

Reference: the `libsparse` sources in the Android Open Source Project
(`sparse_format.h`). Oracles: `simg2img` and `img2simg` in both directions.

Layout, little-endian: a 28-byte header: magic 0xED26FF3A, major version 1,
minor version 0, file header size 28, chunk header size 12, block size
(multiple of 4), total blocks, total chunks, image checksum (written zero;
the CRC32 lives in a trailing chunk). Then chunks, each a 12-byte header:
type, reserved, chunk size in blocks, total size in bytes including the
header, followed by its payload:

| Type | Payload | Total size | Read | Write |
| --- | --- | --- | --- | --- |
| 0xCAC1 RAW | blocks | 12 + blocks x block size | Copy | Data chunks, split at 1 MiB |
| 0xCAC2 FILL | one 32-bit value | 16 | Repeated fill | Never |
| 0xCAC3 DONT_CARE | none | 12 | Zero | All-zero chunks, merged |
| 0xCAC4 CRC32 | one 32-bit value | 16, chunk size 0 | Checked under read-through | With `simg:crc=yes`, last |

Writing: the disk length is rounded up to the block size; the inspection
unit is 1 MiB, a whole number of blocks, so a unit is one `RAW` chunk or
merges into a `DONT_CARE` run; the header's chunk count comes from the first
inspection pass. The CRC32 covers the expanded image with `DONT_CARE` blocks
as zeros. Total blocks and total chunks are 32-bit fields: a disk whose
block count exceeds 2^32 - 1 (16 TiB at the default block size) is refused
at plan time as unsupported by the container, naming the field.

Reading: identity is the magic. Validation then requires major version 1
(another major version is `FSIMG_EUNSUPPORTED`), minor version 0 and the
header sizes, a block size that is a nonzero multiple of 4, every chunk
header within the file with a total size consistent with its type, a
nonzero chunk size for every chunk other than `CRC32`, the sum of chunk
sizes equal to total blocks, and the chunks filling the file exactly, so
the file holds exactly the header's total chunks, as `img2simg` writes it;
total blocks times block size, which two 32-bit fields cannot overflow, is
the disk length. The chunks feed the chunk index in file
order, which is also disk order.

### gzip

Reference: RFC 1952. Oracles: `gzip -t`, `gzip -l`, `zcat`, and images
produced by `gzip` and `pigz`.

Member: identification 0x1F 0x8B, compression method 8, flags (FTEXT, FHCRC,
FEXTRA, FNAME, FCOMMENT; reserved bits must be zero), modification time
(32-bit seconds since 1970), extra flags, OS byte, the optional fields the
flags announce, a DEFLATE stream, CRC32 and length modulo 2^32 of the
decoded bytes. Members may be concatenated.

Writing: one member whose header and trailer fsimg emits itself around a
raw DEFLATE stream from zlib (`deflateInit2` with negative window bits), so
every byte outside the compressed data is fixed by this profile and not by
the zlib build's `OS_CODE`: identification, method 8, flags 0 (no name,
comment or header CRC), modification time as [reproducibility](#reproducibility)
states, extra flags 2 for level 9 and 4 for level 1 and 0 otherwise, OS
byte 3 (Unix); then the CRC32 and modular length of the raw bytes.
`gzip:level` is the DEFLATE level. Selected by `-z` or by the output name's
`.gz` suffix under `-a`, and in the library by the write options'
`compression` naming `gzip`, since compression fills no stack slot. The encoder deflates in 64 KiB
pieces, a zero run from a buffer of zeros, and its deflate state and buffers
are charged to the memory budget.

The output's bound, which the plan charges to the output limit with the raw
intermediate, is the 18 header and trailer bytes plus the larger of zlib's
two conservative DEFLATE bounds for n input bytes: n + n/8 + n/256 + n/512 +
4 for fixed Huffman codes and n + n/32 + n/128 + n/2048 + 7 for stored
blocks (each division rounding down). They hold for any window and memory
level, and the first covers the nine-bit literals zlib-ng's fastest level
can emit, so the bound does not depend on the zlib the library runs
against; the sink refuses a byte past it as an internal error.

Reading: identity is the identification bytes, method 8 and zero reserved
flag bits. The optional fields are bounded reads (FEXTRA
by its 16-bit length; FNAME and FCOMMENT as zero-terminated strings of at
most 64 KiB counting the terminator, discarded); the header CRC16 is checked
when present, and a failure is corrupt. A second member's identification
bytes are checked like the first's, and anything else after a member,
zero padding included, is trailing bytes. Inflate writes through a 256 KiB
buffer, so the spool reservation step is at most 256 KiB, and the hole
rule skips each zero 4 KiB block of an append, counted from its start. Decoding follows the [spool](#stream-input-and-the-spool)
rules.

## Later profiles

Each of these requires its own profile section, or its own design when it
brings new mechanism, before implementation; none is implied by this design:

* **VHDX**: a block-allocation table with its own log and region tables; a
  chunk index fills from the BAT, but the metadata region and log need a
  design.
* **qcow2**: two-level cluster tables, refcounts, optional compression and
  backing files; a chunk index from the L1/L2 tables, with backing files
  refused.
* **VMDK streamOptimized**: compressed grains with embedded markers; a
  sequential writer fits the transcode, a reader needs the grain directory.
* **zstd** and **xz**: whole-file compression with the gzip profile's shape;
  seekable zstd frames could later avoid the spool.
* **Extended MBR partitions**: a bounded EBR chain walk with cycle detection.
* **Multi-segment DMG**: refused until a design covers segment discovery.

## Tests and evidence

Every passing case is paired with a seeded failure that proves the code under
test ran: a fixture mutated in one field, or a fault injected at one step,
must produce the named error. Self round trips establish nothing about
format correctness; the oracle rows do.

| Area | Passing cases | Seeded failures |
| --- | --- | --- |
| Stack parsing | Every legal combination of present slots; `-` in each later slot, shorthand (`fat32/dmg`) and spelled-out (`fat32/-/dmg`) forms parse to the same slots; `i` text re-parses to the same slots | Repeated kind, descending order (`fat32/-/gpt`), `-` in the filesystem slot, unknown name, empty token, trailing `/`; each a syntax error naming the token |
| Settings | Each key at its bounds; derived defaults per filesystem; `disk=` with and without `-s` | Key for an absent layer; out-of-range chunk, block and level; 36-unit name limit crossed; `apfs` on MBR; `disk=` smaller than the volume |
| GPT write | `sgdisk -v` reports no problems on disks of every filesystem; `sgdisk -p` shows the type, name and 1 MiB alignment; 4Kn tables verified with `sfdisk --sector-size 4096`, since `sgdisk` takes the sector size of a file as 512 | A flipped bit in the written entry array fails `sgdisk -v`, proving the oracle sees the table fsimg claims to write |
| GPT read | Disks from `sgdisk` and `qemu-img` open; auto-selection with one filesystem; index, name and type selectors; names compared as UTF-16 units; `gpt:sector=4096` with `-F fat32/gpt` resolves a disk carrying both headers | Header CRC, entry CRC, alternate LBA, usable range, overlapping entries, backup mismatch, truncated backup, protective MBR with no header: each `FSIMG_ECORRUPT` naming the check and never a bare-volume read; two headers at 512 and 4096 without `gpt:sector`: ambiguity |
| MBR | `fdisk -l` and `mtools` read fsimg disks; `sfdisk` disks open; types 0x04, 0x0B and 0x0E accepted; a partition ending at sector 2^32; CHS saturation at 1023 | Missing signature, overlapping entries, entry beyond the disk (`FSIMG_ECORRUPT`), 0xEE entry (routes to GPT), FAT boot sector resembling a table (ambiguity), `mbr:type=0x85` refused |
| DMG write | `hdiutil imageinfo` and `hdiutil verify` accept zlib and `none` images; `hdiutil attach` mounts the FAT volume with and without GPT; `qemu-img convert` yields the raw disk byte for byte; zero runs become zero-fill entries | A flipped data-fork byte fails `hdiutil verify` and fsimg's own verification; `dmg:level=0` and `10` refused |
| DMG read | `hdiutil create` images in UDRO, UDZO and zero-filled variants open and extract, and list as the raw disks `hdiutil convert -format UDTO` and `qemu-img convert` expand them to (`qemu-img` reads DMG but cannot write it); both table shapes; per-table CRC under read-through | Overlapping tables, uncovered sectors, run beyond the data fork, run with zero sectors, run decompressing short and long, run over 16 MiB, bzip2 run (unsupported), unknown type, segment count 2, koly ranges overlapping, plist depth 33, undeclared entity, 5 KiB key; a bare volume whose last sector is a koly whose ranges do not end at the file's end opens as a bare volume |
| VHD | `qemu-img check` and `info` report fixed VHD of the declared size; `vhd:align=1M` lengths; footer checksum; creator fields and original size as written | Flipped checksum (`FSIMG_ECORRUPT`, not a raw read), dynamic type 3 (`FSIMG_EUNSUPPORTED`), size mismatch with file length (no VHD claim: the file is whatever its prefix claims, or unrecognised) |
| simg | `simg2img` expands to the raw disk byte for byte; `img2simg` output opens; `crc=yes` chunk verified | Chunk total mismatch, block sum mismatch, zero-size RAW chunk, FILL with wrong size, CRC mismatch under read-through, block size not a multiple of 4, 2^32 blocks refused at plan time |
| gzip | `gzip -t` accepts output; `zcat` yields the raw file; header bytes fixed (OS 3, flags 0, XFL by level); `gzip` and `pigz` input spools; concatenated members; MTIME clamp at an epoch of -1 and 2^32 | Trailing garbage, CRC mismatch, length mismatch, reserved flag bit, truncated member, 65 KiB FNAME; `-z` on a raw file fails as an absent signature |
| Spool | Stdin of a raw, DMG and gzip image through the coordinator's pipe; all-zero input leaves holes (physical blocks sampled); limit exactly reached; a raw stream whose prefix also passes for gzip is ambiguous and `-z` or `-F` resolves it | A gzip bomb failing at the step that crosses the limit with `FSIMG_RESOURCE_SPOOL`, before the host file grows past it; no spool descriptor; spool descriptor not a fresh empty file; a terminal on stdin refused |
| Transcode | Output bytes identical with hole knowledge consulted and ignored, across budgets and chunk-cache sizes; a sparse raw intermediate read only where its record marks writes; raw intermediate absent from every directory after success and failure; output-limit accounting of both files, in the staging directory and in the scratch tree | Short write, flush failure and cancellation at each chunk: staged files removed, nothing published, stream reports incomplete output after the first byte; a sink needing a raw intermediate without one, or with a descriptor that is linked, non-empty, not regular or read-only, fails before any mutation; `cf -` with a terminal on stdout refused |
| Chunk cache | Sequential reads of a UDZO image decode each chunk once; a pattern confined to four chunks decodes each once; decoded bytes appear in the work counter | A pattern alternating across five maximum chunks pays one decode per read and stops at `-l work=` with `FSIMG_RESOURCE_WORK`, and without a work limit at the decode bound; a 16 MiB memory budget fails the open as a memory limit |
| Wrap, unwrap, compose | Wrap then unwrap is the identity; compose of three volumes lists three partitions with the given names and types in `sgdisk`; inputs read once; a DMG input's zero run composed without a read or a decode, as the [hole design](design-holes.md#tests-and-evidence) details | Aliased input and output; five partitions on MBR; unrecognised volume without `type=` |
| Create then verify | Verification runs on the staged file before publication | A fault injected into the staged file after transcode discards it, publishes nothing, and names the layer; `cW` with `-f -` is a syntax error |
| Reproducibility | Byte-identical output across host order, budget, scratch placement, cache size and hole knowledge for every container, under `compress=none` and under zlib with one zlib version; identifiers follow the derivation under `-R` | Different epochs differ only in the derived fields; an unset identifier without `-R` fails in the library with the key named; zlib version reported |
| Fuzzing | koly, plist, mish, GPT, MBR, VHD footer, simg header and gzip header parsers under sanitizers with small budgets | Corpus seeded from every fixture above |

Oracle availability is reported, not assumed: a missing `hdiutil`, `sgdisk`
or `simg2img` skips its gate explicitly, and the release gate requires each on
its platform.
