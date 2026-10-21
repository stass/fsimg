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
* **Container**: a file format that stores a disk with tables, framing and
  checksums of its own: a disk-image format (DMG/UDIF, fixed VHD, Android
  sparse image (simg)) or a loader's package around one volume (the QNX boot
  image, `ifsboot`; the Harman-Becker `hbcifs` header). "Container" alone
  always means this. The APFS container, the APFS structure that holds APFS
  volumes, is always written out as "APFS container" and is out of scope
  here.
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
| Layer descriptor, one per layer | Probe with its prefix table, read-side open (peel), write-side plan/encode/transcode with the file settings its profile declares, verification, settings and capability record |
| Image (`lib/layers/image.c`) | Read-side peeling order, detection and forcing, partition enumeration and selection, the spool and the spool-backed peel, the layer verification report |
| Container adapter and chunk index (`lib/layers/chunks.c`, `lib/meta/`) | Disk-offset to chunk resolution, the decompressed-chunk cache, chunk validation |
| Disk plan (`lib/layers/plan.c`) | The exact-volume stack rule, disk sizing around a volume, partition placement, partition types and names, identifiers, the plan's disk and partition report |
| Layered output (`lib/layers/output.c`) | The scheme tables written into the staged raw disk, the assembled disk, the transcode pass, the sequential sink, raw-intermediate lifetime |
| Output coordinator (`lib/output/`) | Staging directory, publication, durability, unchanged |
| Filesystem drivers | The volume only; they never see a layer |

Each obligation has exactly one owner. A driver cannot reach a container or a
partition table, and a layer cannot reach a filesystem driver except through
the bounded probe used for partition auto-selection and default partition
types, and the [exact-volume](#exact-volumes) fact the disk plan reads from
its descriptor.

## The stack

An image is a stack with three slots in fixed order, each at most once:

| Slot | Kind | Names |
| --- | --- | --- |
| 1 | Filesystem | `fat12`, `fat16`, `fat32`, `exfat`, `qnx6`, `qnx4`, `ifs`; reserved: `apfs`, `hfs` |
| 2 | Scheme | `gpt`, `mbr` |
| 3 | Container | `dmg`, `vhd`, `simg`, `ifsboot`, `hbcifs` |

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

### Exact volumes

A filesystem descriptor may declare an **exact volume**: its volume must
end where the slot holding it ends, because full verification fails a
longer one. `ifs` declares it, under the ImageFS
[volume-length rule](design-ifs.md#volume-layout). Two layers can make a
slot longer than its volume: every scheme, since a partition is whole
sectors, and a container that rounds the disk up to a granularity above
one byte, which is how a container pads (DMG and VHD to a 512-byte
sector, simg to its block). A create or wrap stack that names such a
filesystem and puts either below it is `FSIMG_EUNSUPPORTED`, naming the
filesystem and the first such layer (`ifs/gpt`, `ifs/vhd`), when the disk
plan resolves the stack, before any byte is written: a scheme before any
container setting is resolved, a container once its settings give its
granularity. The property is the container's resolved granularity, not a
list of names, so a container added to the registry is judged by how it
sizes the disk. Compression encodes the disk byte for byte and never pads:
`ifs` with gzip is written. A
[carried-volume](#prefix-tables-opaque-sections-and-carried-volumes)
container declares a granularity of 1: it packages the volume at exactly
its length, so this rule passes for it with no case of its own. Its
format may still need a length rule: under `none` an `ifsboot` body's
length is a multiple of 4, because `stored_size` is. That rule is the
carried filesystem's own invariant. The filesystem's layout guarantees it
on create, the wrap's verification of its input establishes it, and the
container's bound refuses a disk that breaks it, naming the field. Since
this rule refuses any scheme below `ifs` first, a scheme inside a carried
container around `ifs` is refused here, naming the scheme, before the
carried-volume rule applies.
Reading is unaffected: a disk made otherwise, with
an ImageFS in a partition, still reads, and its full verification reports
a volume longer than the image as the filesystem's design says.

### Layer settings

A setting without a layer qualifier targets the filesystem: `oem`,
`sector`, `cluster`, the QNX6 `endian`, `uuid`, `inodes`, `reserve`, the
QNX4 `inodes`, and the IFS `endian` (shared with QNX6), `mountpoint`,
`boot1` to `boot4` and `script`.
A layer setting is qualified by its layer name: `gpt:type=esp`,
`dmg:compress=none`, `gzip:level=9`. A qualified key for a layer absent from
the stack fails; fsimg never guesses the target of a bare key. On read the
stack is not known until detection runs, so a read-side key is accepted only
for a layer the forced stack names: `gpt:sector=4096` needs `fat32/gpt` or
`gpt` in the stack, and a key for a layer left to detection fails before any
image byte is read. The per-layer keys, types, defaults and validation are in
each [profile](#layer-profiles), summarised in the [settings table](#settings).
In the library every layer setting is a typed field of
`fsimg_layer_options_t`, except a [file setting](#file-settings-and-sections),
whose value is a byte string supplied as an I/O object beside that record;
a layer's capability record declares the settings it
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
| Probe | Claims a disk from its prefix | Claims a file from its prefix, at offset 0 or at an offset of its closed [prefix table](#prefix-tables-opaque-sections-and-carried-volumes), and from its last 512 bytes | Claims a file from its prefix |
| Peel (read) | Enumerates partitions as checked slices of the disk | Yields a read-only positional I/O object over the disk, backed by the chunk index or, for one unframed stream, by the [spool](#spool-backed-peel) | Decodes a sequential stream into the spool |
| Plan (write) | Derives the disk length and the volume offset from the volume length and the container granularity | Declares its disk-length granularity | None |
| Encode (write) | Writes its tables at fixed disk offsets through bounded metadata writes | Transcodes a disk sequentially into the sink, copying in the [file settings](#file-settings-and-sections) its profile declares | Wraps a sequential sink |
| Verify | Table checksums, consistency, partition bounds and overlap | Container checksums, chunk tiling and bounds; a profile may name fields it cannot verify, which make its [scope incomplete](#verification-scope) | Member checksums and length, at spool time |
| Capability | Settings, read/write flags, sector sizes | Settings, codecs, codec version, the filesystem it carries when it carries a volume | Codec version |
| Suffixes | None | The file suffixes that name it: `.dmg`, `.vhd`, `.simg`; `ifsboot` and `hbcifs` declare none, because `.ifs` names the filesystem they carry | `.gz` |

Probes follow the [bounded probe contract](design-formats.md#format-descriptors-and-profiles):
pure functions of fixed bytes and the input length, no I/O, no allocation,
and a claim only after an identity signature matches. A container probe
additionally receives the last 512 bytes, because DMG and VHD identify
themselves at the end of the file, and uses the input length to tie that
trailer to the file: a trailer whose ranges do not end at the file's end is
not a claim, so a bare volume whose last sector happens to hold one is not
taken for a container. Common code reads the prefix, sized by the longest
prefix any container probe declares, and the tail once per peel.

### Prefix tables, opaque sections and carried volumes

Three facts a container profile may declare extend the contract for the
loader packages:

* **Prefix table.** A container probe may declare a closed table of **boot
  prefixes**: fixed offsets at which its header may sit behind an opaque
  loader, each inside the declared prefix. The probe tests its identity at
  every table offset in table order, and a header is a claim only where a
  field of the header ties it to that offset (`preboot_size` for
  `ifsboot`); the first tie decides. The table is closed: a header at any
  other offset draws no claim, and no probe scans for a signature, because
  a signature inside loader code (the string `imagefs` occurs inside SH-4
  startup code) must never be a claim.
* **Opaque section.** A container may hold an **opaque section**: loader or
  startup bytes that are neither a table nor a chunk. A profile states its
  range and the checksum that covers it. The layer never interprets,
  executes or alters those bytes, reports nothing from inside them, and a
  writer copies them verbatim from the
  [file settings](#file-settings-and-sections) the caller supplies.
* **Carried volume.** A container that packages one volume for a loader
  declares the filesystem it **carries** (`ifsboot` and `hbcifs` carry
  `ifs`). The declaration is one fact with three consequences: a create,
  wrap or read-side stack that puts a scheme, or a different filesystem,
  inside such a container is `FSIMG_EINVAL` naming the container, before
  any byte moves, and a forced scheme or filesystem around a detected one
  is the same error once its probe claims the file, before its disk is
  opened (the error says which of the two the stack named); the
  [scheme step](#peeling-order) inside it runs that
  filesystem's probe alone, as if the stack named it with an empty scheme
  slot; and a wrap probes its input with that filesystem's bounded probe
  and requires its claim. The declaration is a field of the container's
  descriptor, and one check applies it: stack validation for a stack that
  names the container, the disk plan for creation, and the container step
  for a container it detects, at the claim, before the container's open
  reads its payload. A detected container then reads as if the stack had
  named the carried filesystem and an empty scheme slot, so `i` prints
  `ifs/hbcifs` before any volume is selected.

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
2. **Container.** The container probes run on the file's prefix, at offset
   0 and at the offsets of their [prefix tables](#prefix-tables-opaque-sections-and-carried-volumes),
   and on its tail. No claim means the file is the disk. One claim peels:
   the container's tables are validated and the disk becomes a positional
   object backed by the [chunk index](#chunk-index-and-cache), or by the
   spool when the profile's payload is one unframed stream
   ([spool-backed peel](#spool-backed-peel)). The DMG koly trailer, VHD
   footer, simg header, `ifsboot` startup header and `hbcifs` header all
   state the disk length, so the disk object has a fixed length before any
   volume byte is read.
3. **Scheme or bare volume.** The scheme probes and the filesystem probes run
   on the disk's prefix together. A scheme claim enumerates partitions; a
   filesystem claim means a bare volume occupying the whole disk. Inside a
   container that carries a volume only the carried filesystem's probe
   runs, so an ImageFS body can never be an ambiguity with a partition
   table.
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
encodings, such as DMG chunks and the framed chunks of a QNX boot image,
are read in place, never spooled. A container whose payload is one
unframed stream decodes it into the same spool at peel
([spool-backed peel](#spool-backed-peel)).

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

### Spool-backed peel

A container whose payload is one sequential compressed stream with no
framing to index, as `hbcifs`'s is, peels through the spool: its peel
decodes the stream into the spool under the filling rules above, bounded by
the decoded length its header declares, and the disk is then the spool, a
slice of exactly that length over a seekable file. The chunk index is not
involved, as it is not for a fixed VHD. The slice retains the input's
backing identity for alias checks, as every peel result does.

The step-1 spool and the peel's spool are one handed-over file, and an
image has one: the open keeps the descriptor through step 2 and closes it
only when neither step used it. A container's descriptor declares that it
peels through the spool, and the container step gives the spool, in its
input, to that container's open alone, never to another container or to
verification; after a successful open the image keeps the spool until it
is freed, after the disk that reads it, and every other outcome closes it
unless it holds the file. Without a spool
descriptor the open fails as a spool limit of zero, as a gzip input does.
A spool-backed container inside gzip, or arriving as a stream, would need
a second spool; that combination is `FSIMG_EUNSUPPORTED` naming both
layers ("the hbcifs container inside gzip needs a second spool"), with the
container in `error.path`, decided at the container claim before any byte
is decoded. The [open question](#open-questions) weighs a second spool.

The disk a spool-backed peel yields is a read-only object of the decoded
length that reads the sealed spool and carries the input's identity. The
image information reports the profile's codec as the container's
encodings and one chunk, the one stream, as a fixed VHD reports its one
raw run.

The declared decoded length is the decoder's output bound, and it is
reserved against the spool limit in bounded steps exactly as a gzip
member's output is, so a stream whose declared length crosses the limit
fails at the step that crosses it with `FSIMG_RESOURCE_SPOOL`. A stream
that ends before the declared length, would produce more, or does not end
exactly at the end of its payload is `FSIMG_ECORRUPT`, naming the
container at the file offset of the stream position where it went wrong.
The bytes each decoder call consumes and produces are charged to the work
counter, the decoder's buffers to memory, and cancellation is polled at
every step.

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

A container whose leading bytes depend on its encoded length runs the pass
twice in the same way. The `ifsboot` startup header carries `stored_size`,
and the checksum covering the header precedes the body, so under a framed
body the first pass compresses the body to learn its framed length and
writes nothing, its finish writes the leading bytes, and the second pass
emits the chunks. The LZO1X encoder is deterministic
([codec design](design-codecs.md)), so the second pass produces the framed
length the first learned; the encoder checks that one number at the end of
the second pass, and a difference means the input changed between the
passes, `FSIMG_ESOURCE_CHANGED`. A raw body needs no first pass: its length
is the disk's. The alternative, compressing once into a raw
intermediate and copying it behind the header, is not used: wrap has no
raw intermediate, and adding one would add a handed-over file for the
coordinator and every embedding application to supply, a second term in
the output budget and a row in the staging table, to save one pass over a
body of at most 4 GiB.

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
  With a container that [carries a volume](#prefix-tables-opaque-sections-and-carried-volumes)
  the stack has no scheme, and the input must draw the carried
  filesystem's bounded-probe claim; `FSIMG_EFORMAT` names the input
  otherwise.
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

### File settings and sections

A container profile may declare **file settings**: layer settings whose
value is a byte string rather than a number or a name. They are the bytes
the container stores beside the disk and derives from nothing: `ifsboot`'s
startup code and boot prefix. Like `compress`, a file setting is
creation-only: every read mode refuses the key before any open, as it
refuses any creation-only key.

How one is supplied. In the library a file setting is a read-only
`fsimg_io_t` field of `fsimg_layer_inputs_t`, a record the write options
borrow beside the typed settings ([C API](design-api.md#writer)). In the
CLI its value is a host path; the coordinator opens it read-only before
the worker starts, and the worker receives the descriptor, never the name
([CLI design](design-cli.md#stack-settings-and-selection)). The typed
settings stay one flat record on the wire; file settings travel as
descriptors. A file setting of a layer absent from the stack fails as any
setting of an absent layer does.

When one is read. The profile declares each file setting as bounded or
streamed. A bounded one has a fixed small maximum (`ifsboot`'s prefix,
at most the largest prefix-table length); the plan reads it whole, once,
into charged memory the plan owns, checks its bytes there before any sink
opens, and the encoder emits exactly those bytes; the buffer is released
with the plan, on every outcome. A streamed one (the startup code) has
only the plan reading its length; the encoder reads it once,
sequentially, in bounded pieces, when the output reaches its place, and
never holds it whole. Padding a profile adds is written as a count of
zero bytes, never as the tail of a reused buffer.

How one fails. An input is checked against the output for
backing-identity aliases like any other input. A file setting that is a
host file (the CLI's descriptor, or a library caller's descriptor
adapter) is checked for change exactly as a regular-file source is under
the [sources design](design-sources.md#host-scanning-and-reopening):
common input handling records its identity, size and change metadata
before the plan reads it and compares them after the copy, and a
replacement, a size change or a metadata difference is
`FSIMG_ESOURCE_CHANGED` naming the input; the check is not an atomic
snapshot against a hostile concurrent writer. For a caller's callback I/O
object, stability for the borrow is the caller's obligation. A read that
ends short of the planned length is the same error; it emits nothing for
the unfilled tail, and the output is incomplete as for every transcode
failure.

A container's **sections** are the byte strings unwrap writes raw: its
disk, under the disk selector, plus one per file setting it declares,
under the setting's unqualified name (`prefix`, `startup`). A disk-image
container (DMG, VHD, simg) has only the disk. Unwrap takes a section
through the section selector (`FSIMG_SELECT_SECTION` naming it), which no
other operation accepts; a section the container does not declare is
`FSIMG_EINVAL` naming it. The typed values a rebuild needs are the
container's details in the image information, which carry every header
field, so a create or wrap given a container's sections as its file
settings and its reported fields as its typed settings reproduces the
container's bytes around the same disk as far as the profile states.

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
| `ifsboot:startup` | [File setting](#file-settings-and-sections) | None: no startup code | The linked startup code, zero-padded to a multiple of 4; the [profile](#qnx-boot-image-ifsboot) says what the bytes are |
| `ifsboot:prefix` | [File setting](#file-settings-and-sections) | None | The raw boot prefix, at most 1088 bytes, not beginning with the startup signature, zero-padded to a multiple of 4; the padded length is a value of the prefix table: 1088 bytes, or empty, which is none |
| `ifsboot:machine` | integer | None; required | A value of the machine table, which also fixes whether a prefix is present |
| `ifsboot:virtual` | `yes`, `no` | None; required | The virtual bit of `flags1` |
| `ifsboot:startup_vaddr`, `ifsboot:paddr_bias`, `ifsboot:image_paddr`, `ifsboot:ram_paddr`, `ifsboot:imagefs_paddr` | `0x` 32-bit | None; each required | Any value, stored verbatim: target addresses fsimg never uses |
| `ifsboot:compress` | `lzo`, `none` | `lzo` | `none` stores the body raw; see the [profile](#qnx-boot-image-ifsboot) |
| `gzip:level` | integer | 6 | 1 to 9; only when gzip is selected (`-z`, a `.gz` name under `-a`, or the library's write options naming `gzip`) |

Values use the CLI's existing byte-quantity, integer, UUID and boolean
syntax, and a file setting's value is a host path. A key whose layer is
absent from the stack fails as an unknown setting. Duplicate keys are syntax errors. `ifs` has no derived `gpt:type`
or `mbr:type`; a scheme below it on create is refused under
[exact volumes](#exact-volumes).

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
for a compressed record (zlib, LZO1X or UCL NRV2B), and zero otherwise.
DMG and simg both fill it from their tables; a fixed VHD is the degenerate
case, one raw record covering the file before the footer, so its disk
object is simply a slice; and an `ifsboot` boot image fills a
[learned index](#learned-index) by decoding. The index owns:

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
`FSIMG_ECORRUPT`. A framed boot-image chunk is bounded the other way round:
its stored length is a 16-bit field and its decoded length is at most the
profile's 65536. Compressed bytes read and decoded bytes produced are both
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

### Learned index

A stored body with no table, only a framed sequence of chunks whose
decoded lengths vary, as a QNX boot image's is, cannot be indexed from
bytes read at fixed offsets: a chunk's disk offset is the sum of the
decoded lengths before it, and an LZO1X or NRV2B stream states its decoded
length nowhere. The peel therefore **learns** the index: it decodes every
chunk once, in stored order, into a bounded buffer it discards, and
appends one compressed record per chunk with the stored offset and length
it read and the disk offset and length it learned. The same sealed stream,
sort and tiling pass follow, so there is one index path; the tiling is met
by construction and is still run. Opening a compressed boot image thus
costs one full decode, and the chunk cache then serves reads exactly as
for DMG, sized by the largest chunk the pass learned rather than a
declared one: at most four chunks of 65536 bytes for `ifsboot`.

The learning pass is where every framing rule of the profile is enforced,
and the profile's bounds bound the index: the pass refuses a non-final
chunk smaller than the profile's smallest, stops when the decoded total
reaches the declared disk length, and so appends at most the declared
length divided by that smallest chunk, plus one, records
(`imagefs_size / 4096 + 1` for `ifsboot`), a fixed small fraction of the
declared length that never scales with the stored length. Stored bytes
read and decoded bytes produced are charged to the work counter, and the
pass polls cancellation per chunk.

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
that qualification, and so is an `ifsboot` image: its codec is
project-owned and deterministic, and every other output byte comes from
the settings, the file settings or the input.

## Verification scope

Layer verification runs before the filesystem's and reports its own scope:

| Layer | Checks |
| --- | --- |
| gzip | Each member's CRC32 and modular length, at spool time; trailing bytes |
| DMG | koly fields and ranges, the data-fork CRC32 by one sequential pass over the data fork where the koly declares it, the master CRC32 over the tables' checksum values where the koly declares it, each block table's CRC32 over its decoded bytes under payload read-through, chunk tiling and bounds, every chunk decoded once under read-through |
| VHD | Footer checksum and the constant fields (format version, data offset, saved state); the cookie, type 2 and the size tie are identity and already hold at open |
| simg | Header and chunk-header consistency, block totals, CRC32 chunk where present under read-through |
| ifsboot | The startup checksum over the startup section and the stored checksum over the whole stored image, each one sequential sum over the file; `ram_size` against its rule where the [machine table](#qnx-boot-image-ifsboot) says so; the closed tables, the bounds and the framing (every chunk decoded, the terminator, the zero padding, the decoded total) are identity, open and learning-pass checks and already hold |
| hbcifs | The identity, the length tie, the header constants and the exact decode already hold at open; the fields at `0x14` and `0x1c` are unidentified and are reported unverified, so the scope is incomplete |
| GPT | Protective MBR, primary and backup header CRC32 and entry-array CRC32, primary and backup consistency (disk GUID, usable range, entry arrays byte-identical), partitions within the usable range and non-overlapping |
| MBR | Signature, entry field sanity, partitions within the disk and non-overlapping |

Then the selected volume is verified by `fsimg_verify()` as for a raw image.
Partitions not selected are not verified, and the report says so. Ordinary
open already performs every table check: header, trailer and entry
validation, chunk tiling and bounds, partition bounds and overlap, so a
damaged table never yields a volume. Verification adds the passes that read
beyond the tables: the DMG data-fork and master CRC32s, the `ifsboot` sums,
and under read-through the per-table, simg and member checksums with every
chunk decoded.

A profile that holds a field it cannot verify, because no evidence
identifies the field's rule, names that field in the layer report as
**unverified**, and the layer scope is then **incomplete**: every other
check ran and passed, and the report never calls the layer verified. The
profile's descriptor lists those fields; the layer report carries the
list (`unverified`, "0x14,0x1c" for `hbcifs`) and is then not complete,
while verification still succeeds. `W` names the fields beside the
container ("hbcifs container (incomplete: fields 0x14,0x1c unverified)"),
its JSON verify record is `"complete":false` with an `unverified` member,
and its exit status is that of a passing verification. A
container that carries a volume needs no check that its declared decoded
length matches the volume's own length field: the disk it yields is
exactly that length, and the `ifs` driver's verification compares its
`image_size` with its volume length, so no layer reads a filesystem field.

## Dependencies and capabilities

zlib is a required dependency, linked from the system as the
[architecture](architecture.md#dependencies-and-implementation-organization)
permits: gzip and DMG zlib chunks (UDZO) are always available, and configure
fails clearly without it. Other DMG chunk codecs (ADC, bzip2, lzfse, lzma)
are unsupported chunk compression and fail with an explicit error naming the
type; they are never silently treated as zeros. zstd and xz are reserved
compression names for later profiles.

The LZO1X decoder and encoder and the UCL NRV2B decoder are project-owned
code with no knowledge of IFS or of any container, specified in the
[codec design](design-codecs.md): bounded on both sides, deterministic on
the encoding side, charged to the work counter, and always available, so
no boot image needs an optional dependency. The `ifsboot` zlib compression
value is unsupported and fails naming the value, because QNX 6.5.0's
`mkifs` cannot produce it and no fixture exists.

Capability enumeration reports each layer with its kind, read and write
flags, settings, codecs, the filesystem it carries, the zlib version string
and, for the project-owned codecs, fsimg's own release as their version,
through the same immutable registry mechanism as formats. Runtime sandbox
reporting is unchanged.

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
  produces more than declared stops at the buffer end and is corrupt. A
  framed boot-image chunk decodes into a 64 KiB buffer, the profile's
  maximum, and an unframed stream into the spool under its declared length
  and the spool limit; the learning pass that indexes a boot image reads at
  most the stored image and writes at most the declared decoded length,
  both charged to the work counter, and the index it builds is bounded by
  that length.
* **Opaque sections are bytes.** Startup code and boot prefixes are never
  executed, interpreted or reported from inside; a writer copies them from
  the [file settings](#file-settings-and-sections) the caller supplies,
  checked for length and, where a profile states one, a framing check
  (the prefix's leading signature) and never parsed as an executable
  format, and an unwrap writes them back as bytes.
  The address fields of a startup header are target addresses, reported,
  stored from settings verbatim and never used as offsets. A file setting
  is a read-only descriptor the coordinator opened, bounded by the
  profile's length rule and charged to the output it is copied into; the
  worker gains no name authority from it.
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

### QNX boot image (ifsboot)

`ifsboot` is QNX's startup header (`struct startup_header`) and startup
program in front of an ImageFS: the layer that turns an image filesystem
into a bootable QNX image.

Reference: the QNX startup header as the vendor's IPL and startup
documentation describes it; every value a table below accepts comes from
the native images in the [IFS research record](../docs/ifs-research.md)
(S1, the eight PCM 3.1 SH-4 images; S2, the three QNX 6.5.0 x86 images;
E7, `mkifs` compressed output), and a value no native image shows is not
accepted. Oracles: QNX 6.5.0's `mkifs` and `dumpifs` in the guest, the
eleven native boot images against their native manifests, the out-of-tree
codec oracles for the framing, and for a written image a native boot (E12
on x86 in QEMU; the PCM board for SH-4). The container carries `ifs`.

A boot image is an optional **boot prefix**, the **startup section** and
the **stored body**, in that order, and nothing after. Let `B` be the
prefix length, the offset of the startup header; every header offset
below is from `B`, and `startup_size` and `stored_size` count from `B`.
The prefix table is closed:

| Prefix | `B` | `preboot_size` | Observed with |
| --- | --- | --- | --- |
| None | 0 | 0 | Machine 42 (S1); machine 0 (E7) |
| x86 BIOS loader | 1088 | 1088 | Machine 3 (S2) |

The startup header is 256 bytes, little-endian:

| Offset | Size | Field | Use |
| --- | --- | --- | --- |
| 0x00 | 4 | Signature `0x00FF7EEB`, bytes `eb 7e ff 00` | Identity |
| 0x04 | 2 | Version | Closed table: 1 |
| 0x06 | 1 | `flags1` | Closed tables below |
| 0x07 | 1 | `flags2` | Opaque |
| 0x08 | 2 | `header_size` | Identity: 256 |
| 0x0a | 2 | Machine | Closed table below |
| 0x0c | 4 | `startup_vaddr` | Target address: reported, never an offset |
| 0x10 | 4 | `paddr_bias` | Target address: reported |
| 0x14 | 4 | `image_paddr` | Target address: reported |
| 0x18 | 4 | `ram_paddr` | Target address: reported |
| 0x1c | 4 | `ram_size` | `startup_size + imagefs_size` where the machine table says so |
| 0x20 | 4 | `startup_size` | Length of the startup section from `B` |
| 0x24 | 4 | `stored_size` | Length of the stored image from `B` |
| 0x28 | 4 | `imagefs_paddr` | Target address: reported |
| 0x2c | 4 | `imagefs_size` | Decoded body length: the disk length |
| 0x30 | 2 | `preboot_size` | Ties the header to its prefix |
| 0x32 | 206 | Tail | Opaque: zero in S2, six nonzero bytes in S1 |

`flags1` decomposes into bit 0x01 (virtual addressing), bit 0x02 (a
big-endian header, in which every field above is byte-swapped), bits 0x1c
(the compression value) and bits 0xe0 (reserved). Each field has a closed
table, so the accepted `flags1` values are the products of the tables:

| Field | Accepted | Evidence |
| --- | --- | --- |
| Version | 1 | S1, S2, E7 |
| Virtual bit 0x01 | 0 or 1 | 0 in E7's nonbootable output, 1 in S1 and S2 |
| Big-endian bit 0x02 | 0 | A big-endian header stores the signature as `00 ff 7e eb`, which is not the identity and draws no claim; the bit set under a little-endian signature is a contradiction and is unsupported |
| Compression bits 0x1c | 0x00, 0x08, 0x0c, per the compression table | S1, S2, E7; see the table |
| Reserved bits 0xe0 | 0 | Clear in every image |

| Machine | Architecture | Prefix | `ram_size` rule | Writable modes | Evidence |
| --- | --- | --- | --- | --- | --- |
| 42 | SH-4 | None | Checked | None: no boot of a written SH-4 image is recorded | S1: `flags1` 0x09, `startup_size` 53512 |
| 3 | x86 | BIOS loader | Checked | None: no boot of a written x86 image is recorded | S2: `flags1` 0x0d, `startup_size` 75056 |
| 0 | None: nonbootable `mkifs` output | None | Reported, not checked | `none`, `lzo`: no boot applies | E7: `flags1` 0x08 or 0x0c, `startup_size` 260 |

Reading and writing have independent acceptance: every row is read, and
the writable-modes column lists, per machine, the `ifsboot:compress`
values the writer accepts, exactly the modes a native boot of a written
image has passed for that machine ([IFS plan](../docs/ifs-plan.md)). A
mode the column does not list for the resolved machine, the default
`lzo` included, is `FSIMG_EUNSUPPORTED` naming the machine and the mode,
at plan time; the default is never changed silently.

Machine 0 is in the table because it is what `mkifs` writes for a
compressed nonbootable image (`compress=2` or `compress=3` on a buildfile
with no startup program): the one native producer of the chunk framing
fsimg can run, so every native LZO and UCL fixture carries it, and
`dumpifs` lists such images as it lists boot images. Nothing in this layer
depends on bootability; the ImageFS inside reports its own boot
references. The `ram_size` relation is observed only on the eleven boot
images, so for machine 0 the field is reported and not checked.

The **startup section** is `[B, B + startup_size)`: the header, then the
startup code, which is an opaque section, ending in the startup checksum
word. The little-endian 32-bit words of the section sum to zero modulo
2^32. `mkifs`'s nonbootable output has the 260-byte minimum: the header
and the checksum word alone (E7). In S1, two boot images built from one
startup program differ inside the section only in the derived header fields
and its last word, which locates the checksum word in bootable sections
too.

The **stored body** is `[B + startup_size, B + stored_size)` and its shape
follows the compression value:

| Value | Body | Read | Write |
| --- | --- | --- | --- |
| 0x00 | The ImageFS bytes, `imagefs_size` long; `stored_size = startup_size + imagefs_size`; no stored trailer: the ImageFS's own checksum closes the whole-image sum | Slice | `ifsboot:compress=none` |
| 0x04 | zlib | Unsupported, naming the value: 6.5.0 `mkifs` fails to produce it (E7) and no fixture exists | Never |
| 0x08 | LZO1X chunks in the framing below | Learned index | `ifsboot:compress=lzo` |
| 0x0c | UCL NRV2B chunks in the framing below | Learned index | Never: no encoder |
| Other | | Unsupported, naming the value | Never |

The **framing** (E7, S1, S2) is a sequence of chunks, each a 16-bit
big-endian stored length followed by that many stored bytes, ended by a
zero length; zero padding up to `B + stored_size - 4`; and the 4-byte
stored checksum word, the last word of the stored image. `stored_size` is
a multiple of 4. The little-endian 32-bit words of the whole stored image
`[B, B + stored_size)` sum to zero modulo 2^32; since the startup section
sums to zero on its own, so does the body with its padding and trailer.
Every chunk decodes to at most 65536 bytes, and every non-final chunk to
at least 4096, the smallest non-final chunk any native image shows (the
S1 `PCM3_IFS1` family has pairs of 4096 and 61440; every other non-final
chunk is 65536). A stored length is at most 65535 by its width; the
largest observed is 65059.

Reading: the probe claims on identity at each prefix-table offset in table
order, inside the 1344-byte prefix it declares: the signature and
`header_size` 256 at `B`, tied to the prefix by `preboot_size == B`. The
first tie decides, and no other offset is ever examined. Validation then
requires, as unsupported, a value outside the version, `flags1`, machine
and compression tables; and, as corrupt, a bound violation: `startup_size`
at least 260 and a multiple of 4, `stored_size` a multiple of 4 and at
least `startup_size`, `B + stored_size` equal to the file length (a stored
image followed by anything is not a boot image under this profile, which
is what refuses an IFSCSA array, a signature or an S-record wrapper),
under compression 0x00 `stored_size` equal to `startup_size +
imagefs_size`, and under a framed value `stored_size` at least
`startup_size + 8`, room for the terminator, its padding and the trailer,
so that the trailer offset `B + stored_size - 4` lies at or after the
body start and every range below is computed without underflow. The disk
length is `imagefs_size`, and `i` prints the stack as `ifs/ifsboot`.

Peel of an uncompressed body is the slice `[B + startup_size, B +
stored_size)`. Peel of a framed body is the
[learning pass](#learned-index), which reads the framing once from `B +
startup_size` and enforces:

* every length word, the terminator's included, and each chunk's stored
  bytes lie entirely before the trailer at `B + stored_size - 4`; a word
  or chunk that would reach it is corrupt, and the padding range from the
  terminator's end to the trailer is therefore never negative;
* each chunk decodes through the profile's codec into a 65536-byte buffer
  whose bound is also the bytes left to `imagefs_size`; a chunk that would
  produce more stops decoding and is corrupt, as is a chunk whose stream
  does not end exactly at its stored length;
* every non-final chunk, known as such when the next length is nonzero,
  decoded to at least 4096 bytes; a smaller one is unobserved and is
  `FSIMG_EUNSUPPORTED`;
* the sequence ends with a zero length, and the decoded total then equals
  `imagefs_size`; a missing terminator or a different total is corrupt;
* the bytes from the terminator to `B + stored_size - 4` are zero.

Each chunk becomes one compressed record with the codec number of the
compression value, and the chunk count is at most `imagefs_size / 4096 +
1`. A `dumpifs -u` copy keeps the compression value with a decoded body
(S2); under this profile it is a boot image whose body does not decode,
reported corrupt at the first chunk, and its ImageFS is read as a byte
slice of the file instead.

Verification adds the two sums, each one sequential pass over the file:
the startup checksum over `[B, B + startup_size)` and the stored checksum
over `[B, B + stored_size)`, each `FSIMG_ECORRUPT` naming its section when
nonzero; and `ram_size == startup_size + imagefs_size` where the machine
table says so. The tie between `imagefs_size` and the ImageFS's
`image_size` is the driver's check of `image_size` against its volume
length ([volume layout](design-ifs.md#volume-layout)), as the
[verification scope](#verification-scope) states.

Info reports the prefix length, every header field above (the version,
the virtual bit, the compression value, the machine by number, the five
target addresses, `ram_size`, `startup_size`, `stored_size`,
`imagefs_size` and `preboot_size`), the two opaque fields as `flags2` and
the count of nonzero tail bytes, so that a rebuild's one difference from
an H3 image is visible, the codec, and the chunk statistics: the chunk
count, the stored body length, the smallest and largest decoded chunk and
the largest stored chunk. The numbers are the container's details, named
values in this order: `prefix_bytes`, `version`, `virtual` (0 or 1),
`compression` (the value), `machine`, `startup_vaddr`, `paddr_bias`,
`image_paddr`, `ram_paddr`, `ram_size`, `startup_size`, `stored_size`,
`imagefs_paddr`, `imagefs_size`, `preboot_size`, `flags2`,
`tail_nonzero`, `body_bytes` (`stored_size - startup_size`),
`min_chunk_bytes`, `max_chunk_bytes` and `max_stored_chunk_bytes` (a
chunk's stream, without its length word). A
raw body is one raw chunk of `imagefs_size` bytes. A detail holds a
number, so the compression value's name is the image's codec list
(`raw`, `lzo1x` or `nrv2b`), the chunk count is its chunk count, and the
machine's architecture is the machine table's.

Writing packages an ImageFS volume the way `mkifs` does
([research record](../docs/ifs-research.md#startup-and-boot-file-sources-e13),
E13): the header is synthesized, the startup code and the boot prefix are
opaque bytes copied from inputs, and nothing in either is parsed. A
create stack `ifs/ifsboot` transcodes the staged raw volume the `ifs`
writer emitted; a wrap packages an existing volume; the encoder is the
same in both. Its inputs are the profile's two
[file settings](#file-settings-and-sections) and its typed settings:

| Input | Kind | Value |
| --- | --- | --- |
| `ifsboot:startup` | File setting | The startup code, linked at its address, as it follows the header in an image: what unwrap's `startup` section returns. Linking is the caller's job, as `mkifs` leaves it to the linker named in the buildfile; fsimg takes the result. Absent, there is no code and the section is the header and its checksum word, the 260 bytes `mkifs` writes for a buildfile without a startup program (E7) |
| `ifsboot:prefix` | File setting | The boot prefix, the raw bytes before the startup header: what unwrap's `prefix` section returns. `mkifs` takes it from the loader segment of the boot file `[virtual=...]` selects, after the file's attribute block (E13); a user takes it from `bios.boot` the same way or from unwrap |
| `ifsboot:machine` | Setting, required | A value of the machine table |
| `ifsboot:virtual` | Setting, required | The virtual bit of `flags1` |
| `ifsboot:startup_vaddr`, `ifsboot:paddr_bias`, `ifsboot:image_paddr`, `ifsboot:ram_paddr`, `ifsboot:imagefs_paddr` | Settings, each required | The five target addresses |
| `ifsboot:compress` | Setting, default `lzo` | The body's compression value: 0x08 under `lzo`, 0x00 under `none` |

Each file input is padded with zeros to a multiple of 4, as `mkifs` pads
the loader (E13: 1085 loader bytes become the 1088-byte prefix). The
padding is a count of zero bytes the encoder writes after the file's
bytes, and the derived sizes count it; a length that would overflow its
32-bit field is refused before rounding. The padded prefix length must be
a value of the prefix table, so it is 1088 bytes or none, and an empty
file is none.

The header is written whole from three sources, the fixed identity, the
settings and the derivations, and the table says for each field which,
and what `mkifs` does in its place: the settings stand where `mkifs`
reads a buildfile attribute or the startup ELF, which fsimg does not
parse.

| Field | Source on write | What `mkifs` does (E13, E7, S1, S2) |
| --- | --- | --- |
| Signature, version, `header_size` | Fixed: the identity, 1 and 256 | The same constants |
| `flags1` | The virtual bit from `ifsboot:virtual`, the compression bits from `ifsboot:compress`, the big-endian and reserved bits zero | The `virtual=`, `+compress` and `+bigendian` attributes |
| `flags2` | Zero | Zero in every 6.5.0 image |
| Machine | `ifsboot:machine` | The startup ELF's `e_machine`: 3 (`EM_386`), 42 (`EM_SH`), 0 without a startup program |
| `startup_vaddr` | The setting, verbatim | The linked startup's entry point, an address inside the code |
| `paddr_bias`, `image_paddr`, `ram_paddr` | The settings, verbatim | The buildfile's address attributes; `image_paddr` is the image address plus the prefix length |
| `imagefs_paddr` | The setting, verbatim | Zero in E12; its rule is not identified |
| `ram_size` | `startup_size + imagefs_size` | The same sum, in every boot image |
| `startup_size` | 256 plus the padded code plus the 4-byte checksum word | Computed |
| `stored_size` | `startup_size` plus the body's length: the framed length the first pass learns under `lzo`, `imagefs_size` under `none` | Computed |
| `imagefs_size` | The disk length, the ImageFS's `image_size` by the carried-volume rule | Computed |
| `preboot_size` | The padded prefix length | Computed |
| Tail | Zero | Zero in every 6.5.0 image; six nonzero bytes of unknown origin in the H3 images, which only a rebuild with format-specific machinery could reproduce, so fsimg does not |

The addresses and the machine have no defaults: fsimg does not know where
a loader puts an image, and a guessed address boots nothing.

Validation is the plan's, before any sink opens, with one emission
invariant after it. The plan checks every argument: the settings against
their tables, the mode against the machine's writable modes, the startup
code's
length, and the prefix, which it reads whole into its own buffer (the
prefix is bounded by the largest prefix-table length, 1088 bytes), pads,
and checks: the padded length is a value of the prefix table, and the
bytes at offset 0 are not the startup signature, the probe's identity, so
the reader, which takes the first tie in table order, can claim only at
`B`. The plan then assembles the header's setting fields and runs the
read side's closed-table check, which pairs each machine with its prefix.
Nothing below this point refuses an argument. The emission invariant:
before the first output byte, the encoder assembles the buffered prefix
and the complete header, `B + 256` bytes, and runs the read side's whole
header decode on them with the real output length, known at plan time
under `none` and after the first pass under `lzo`, where `stored_size` is
the actual framed length the pass learned, not the bound; the decode must
succeed and must tie at offset `B`, else `FSIMG_ESTATE`, since every
input to it was checked by the plan. The plan's output bound is
`B + startup_size` plus the body's bound: `imagefs_size` under `none`,
and under `lzo`
`imagefs_size + imagefs_size / 255 + 20 * (imagefs_size / 65536 + 1) + 12`,
the sum over chunks of each chunk's stored length (a chunk of `n` decoded
bytes costs at most `n + n / 255 + 8` stored bytes unsplit and
`n + n / 255 + 17` split, by the encoder's
[worst-case bound](design-codecs.md#worst-case-bound)), the terminator,
the padding and the trailer; a `stored_size` or `ram_size` past 32 bits
at that bound is refused at plan time.

The output is the prefix, the startup section and the body, in file
order, the prefix from the plan's buffer and the rest from sequential
sources read once:

* the prefix, the bytes the plan buffered and checked, padding included;
* the header, as derived above;
* the code, the startup file's bytes and their padding, copied in bounded
  pieces and summed in little-endian words as they pass, then the startup
  checksum word, set so that the section sums to zero;
* the body, under `none` the disk copied verbatim and under `lzo` chunks
  of 65536 decoded bytes with a shorter last chunk, each encoded by the
  project's deterministic LZO1X encoder. A chunk of `n` decoded bytes
  whose encoding is longer than 65535 bytes, the most a 16-bit stored
  length holds, is emitted as two chunks of `ceil(n / 2)` and
  `floor(n / 2)` decoded bytes, in that order; the second keeps the
  original's place, so it is final exactly when the original was. The
  rule is deterministic and never produces a non-final chunk below the
  profile's 4096 minimum: the encoder's output is at most
  `n + n / 255 + 6` bytes, so only a chunk longer than 65273 bytes can
  exceed 65535, and both halves of such a chunk exceed 4096, whichever is
  final; a final chunk has no minimum. The sequence ends with the zero
  length, zero padding to a 4-byte boundary, and the trailer, the last
  word of the stored image, set so that the whole stored image sums to
  zero: the startup section sums to zero on its own, so the trailer is the
  negated sum of the stored body's words, which the encoder accumulates
  as it writes them, from `B + startup_size`, a word boundary.

In both modes the encoder keeps one accumulator over the plain body, the
disk's little-endian words as the transcode hands them, and at the end of
the copy requires it to be zero, the ImageFS's own checksum property;
a nonzero sum is `FSIMG_ECORRUPT` naming the body, or
`FSIMG_ESOURCE_CHANGED` for a wrap, whose input verified before the copy.
So the body's checksum is checked by the encoder whatever the mode, a
create's staged volume included; under `lzo` the stored trailer is a
second accumulator over the stored bytes, and under `none` the plain sum
is the stored sum.

Under `lzo` the [transcode](#transcode) runs twice: the first pass encodes
every chunk and keeps one number, the framed length, and writes nothing;
its finish derives `stored_size`, writes the prefix, the header, the code
and the startup checksum; the second pass emits the chunks and checks its
framed length against the first pass's at its end. Under `none` the
leading bytes are written before the first chunk and the disk is read
once. The encode record (`fsimg_encode_t`) carries the file inputs to the
encoder beside the resolved settings. The transcode hands an all-zero
unit to the encoder as a NULL chunk; the framing has no zero record, so
the encoder encodes or copies zeros like any other bytes. The encoder
holds one 65536-byte input chunk, one output buffer of the encoder's bound
for that length, the encoder's 64 KiB table and one 64 KiB file-copy
buffer and the plan's prefix buffer, each charged to memory; the framed
length and the two running sums are the only state carried between
passes, so no table of chunks is kept anywhere. Stored and decoded bytes
are charged to the work counter and cancellation is polled at every
chunk, as for every transcode. The output is reproducible as the
[reproducibility](#reproducibility) section states: nothing in it is
host-dependent.

Before any output is produced, the wrap operation, not the container,
opens the input as an `ifs` volume and verifies it at structural scope,
which includes the ImageFS checksum; a corrupt input is `FSIMG_ECORRUPT`
naming the input, under `lzo` and `none` alike. A verified ImageFS is at
most 2^32 - 4 bytes long and a multiple of 4, so every length the layout
requires holds for it. A create's staged volume is the `ifs` writer's
own and is not verified before the copy; the encoder's plain-body sum
covers its checksum, and `cW` verifies the staged image before
publication under [create then verify](#create-then-verify).

Unwrap writes the profile's [sections](#file-settings-and-sections): the
`disk` selector gives the ImageFS, `prefix` the bytes `[0, B)`, empty
without a prefix, and `startup` the code `[B + 256, B + startup_size - 4)`.
The typed settings of a rebuild are the image's details, which carry
every header field under the settings' names (`machine`, `virtual`, the
five addresses, `compression`). A wrap of that disk with those sections
and those values reproduces the file byte for byte when the body is raw
and `flags2` and the tail are zero (E12's `cAu`): every byte is then
copied or derived by a rule the native image satisfies too. A nonzero
`flags2` or tail (the H3 images) is the one difference in the startup
section, and info's `flags2` and `tail_nonzero` details show it. When the
body is framed, the body is this encoder's rather than the native one's,
so `stored_size`, the checksum words and, when the codec differs, the
compression bits differ as well; the disk reads back identical in every
case. The X1 images and every `+compress` build of 6.5.0 report
compression `ucl`, a codec the writer refuses, so their rebuild chooses
`lzo` or `none`, and byte identity holds only for raw natives.

Refusals, each the plan's, before any sink opens:

* a required setting unset, or a value outside its table: `FSIMG_EINVAL`
  naming the key;
* a mode the machine table does not list as writable for the resolved
  machine: `FSIMG_EUNSUPPORTED` naming the machine and the mode;
* a prefix whose padded length is not in the prefix table, or whose
  bytes begin with the startup signature: `FSIMG_EINVAL` naming the key;
* a header the read side's closed-table check refuses, such as a machine
  without its prefix or a prefix without its machine: `FSIMG_EINVAL`
  naming `ifsboot:machine` with the reader's reason;
* an `ifs` byte order other than little, the resolved `endian=` setting
  of a create or the ImageFS flags (0x05) of a wrap's input:
  `FSIMG_EUNSUPPORTED` naming the byte order, one precondition of the
  plan for create and wrap alike under both compression modes, because
  no native image pairs a little-endian startup header with a big-endian
  body and the profile writes no other header;
* a `stored_size` or `ram_size` past 32 bits at the bound:
  `FSIMG_EUNSUPPORTED` naming the field. The bound also refuses a disk
  longer than 2^32 - 4 bytes, or under `none` not a multiple of 4, naming
  `imagefs_size`; a verified ImageFS satisfies both, so for a create and a
  wrap that check is an invariant of the plan, kept because the bound is
  the container's contract with the disk plan;
* an input that draws no `ifs` claim: `FSIMG_EFORMAT` naming the input;
* an input that fails its structural verification: `FSIMG_ECORRUPT`
  naming the input;
* a file setting or the input aliasing the output, as for every input.

After the first byte: a file setting the change check finds replaced,
resized or modified, a startup read that ends short, a second pass whose
framed length differs from the first, and a wrap's body whose plain sum
is not zero are `FSIMG_ESOURCE_CHANGED` naming the input; a create's body
whose plain sum is not zero is `FSIMG_ECORRUPT` naming the body; a header
decode that fails or ties elsewhere than `B` is `FSIMG_ESTATE`. The output
is then incomplete as for every transcode failure.

### Harman-Becker IFS container (hbcifs)

Reference: none public. The layout is the one file that carries it,
`PCM3_IFS2.ifs` on the PCM 3.1 update disc, as the
[IFS research record](../docs/ifs-research.md) (S1) measures it; its
payload decodes, through the out-of-tree `liblzo2` oracle, to a nonbootable
ImageFS of exactly the declared length. Oracles: that decode and the
image's native manifest. The container carries `ifs`. There is no writer.

Layout: a 64-byte header, then the payload, one unframed LZO1X stream that
runs to the end of the file. Header fields are little-endian:

| Offset | Size | Field | Use |
| --- | --- | --- | --- |
| 0x00 | 8 | `hbcifs` and two NUL bytes | Identity |
| 0x08 | 4 | Decoded length | The disk length; the decoder's output bound |
| 0x0c | 4 | Payload length | Must equal the file length minus 64 |
| 0x10 | 4 | Unidentified; 32 in the one image | Closed table: 32 |
| 0x14 | 4 | Unidentified; neither the CRC-32 nor the additive sum of the payload | Reported; unverified |
| 0x18 | 4 | Unidentified; bytes `02 01 01 00` | Closed table: those bytes |
| 0x1c | 4 | Unidentified; bytes `49 4f 33 f3` | Reported; unverified |
| 0x20 | 32 | Zero | Closed table: zero |

Reading: identity is the eight bytes at offset 0. Validation then requires
the payload length equal to the file length minus 64 and a nonzero
decoded length, since an ImageFS starts with a header, else
`FSIMG_ECORRUPT`; and the values the closed table names at 0x10, 0x18 and
0x20, else `FSIMG_EUNSUPPORTED` naming the field, because a second
producer variant is unobserved and is reported rather than guessed at. The
fields at 0x14 and 0x1c vary per image for all anyone knows, so they are
decoded, reported by info and never checked. `i` prints the stack as
`ifs/hbcifs`.

Peel is the [spool-backed peel](#spool-backed-peel): the payload is decoded
through the project's LZO1X decoder into the spool with the decoded length
as the output bound, reserved against the spool limit in bounded steps,
and the stream must end exactly at the end of the payload, where the
research record places the end marker, and produce exactly the decoded
length. The disk is the spool, `decoded length` bytes long.

Verification repeats nothing: the identity, the tie, the closed table and
the exact decode hold at open. It reports the fields at 0x14 and 0x1c as
unverified, so the layer scope is incomplete, as the
[verification scope](#verification-scope) states, until evidence
identifies the checksum field; the tie between the decoded length and the
ImageFS's `image_size` is the driver's check, as for `ifsboot`.

Info reports the decoded and payload lengths, the four unidentified fields
as hexadecimal values, and the codec: the image information's container
details `decoded_bytes`, `payload_bytes`, `field_0x10`, `field_0x14`,
`field_0x18` and `field_0x1c`, each a 32-bit field's value, which `i`
prints in hexadecimal as it prints every container detail, and `lzo1x` as
the container's encodings. The descriptor declares no writer, so a
creation stack naming `hbcifs` is refused naming it, and the capability
record lists it as read and verified.

Decoding reads the payload in 64 KiB pieces and writes through a 64 KiB
window behind 64 KiB of history, more than the 49151 bytes an LZO1X match
reaches back, appending each window to the spool: the buffers are charged
to memory, and each append is one reservation step.

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
| ifsboot read | The eight H3 and three X1 boot images list and extract against their native manifests with no `dumpifs -u` step, and verify; `mkifs` LZO and UCL nonbootable output (machine 0) opens; generator-built images with opaque startup bytes for each prefix and each compression value, including 0x00; `i` prints `ifs/ifsboot` and the startup fields; sequential and confined reads decode each chunk once | A header at an offset outside the table, and `preboot_size` not equal to its offset: no claim; the signature inside SH-4 startup code: no claim; each closed-table field mutated: `FSIMG_EUNSUPPORTED` naming it; each bound mutated, `stored_size` equal to `startup_size + 4` under a framed value, trailing bytes after the stored image, the zlib value: the named class; a length word or chunk reaching the trailer, a missing terminator, a non-final chunk below 4096 (unsupported), a chunk decoding past 65536 or past `imagefs_size`, a decoded total short of `imagefs_size`, nonzero padding, each sum flipped: `FSIMG_ECORRUPT` naming the check; a body of minimum-size chunks longer than the index bound fails before scratch grows past it; a pattern alternating across five chunks stops at `-l work=`; a `dumpifs -u` copy is corrupt at the first chunk |
| ifsboot write | A create `ifs/ifsboot` and a wrap of a standalone writer image, each with generator-built startup code and settings, peeled again, yield that image's bytes under `lzo` and `none`; the output verifies, both sums and `ram_size`, and `i` reports the settings' values and zero `flags2` and `tail_nonzero`; a 1085-byte prefix is padded to the 1088-byte prefix and no prefix is accepted without one, on a test build whose machine table lists every machine as writable, since the plan check and the emission are the same code for every machine; absent startup code gives `startup_size` 260; code of 4001 bytes is padded to 4004; the 1085-byte prefix copied through a poisoned buffer yields three zero padding bytes; the prefix buffer is released on success and on every failure; byte-identical output across hosts and budgets; an incompressible 65536-byte chunk and an incompressible final chunk of 65400 bytes each split into two halves that read back; every section of each native image, H3 included, unwraps to the bytes the profile names; for each native image whose machine and mode the table lists as writable, its disk wrapped again with those sections and the values `i` reports is byte-identical when the native body is raw and `flags2` and the tail are zero (E12's `cAu`) and otherwise identical in the prefix, the code and every header field but `stored_size`, the compression bits, the checksum words and a nonzero tail, with a disk that reads back identical; the native gates in the [IFS plan](../docs/ifs-plan.md) | Each required key unset, named; machine 42 under `none` and machine 3 under `lzo` refused naming the machine and the mode; on the test build, machine 3 without the prefix and machine 0 with it; a 1084-byte and a 1089-byte prefix; a prefix beginning with the startup signature, refused by the plan before the sink opens, with a caller sink that records every open and resize; a create with `endian=big` and a wrap of a big-endian body (`FSIMG_EUNSUPPORTED` naming the byte order) under both modes; a file-setting key on `t`, `x`, `i` and `W` refused before any open; a startup or prefix file replaced, grown or truncated between plan and emission (`FSIMG_ESOURCE_CHANGED` from the change check), the truncated one emitting no padding tail; an input without an `ifs` claim; a wrap input with a flipped ImageFS checksum word refused as `FSIMG_ECORRUPT` before any output byte; a create whose staged volume's checksum word a fault flips fails at the end of the copy with `FSIMG_ECORRUPT` naming the body under `lzo` and under `none`, and a wrap input changed after verification with `FSIMG_ESOURCE_CHANGED`; the input changed between the two passes (`FSIMG_ESOURCE_CHANGED`); a fault that makes the emission decode tie at 0 is `FSIMG_ESTATE`; a section name the container lacks, the section selector given to `select()`, and a section of a DMG; a startup or prefix file aliasing the output; `ifsboot:compress=ucl` refused naming the key; one address setting altered rebuilds an image whose `i` shows the altered value and whose startup checksum still verifies; an H3 image's `tail_nonzero` is 6 and a rebuilt one's 0 |
| hbcifs | `PCM3_IFS2.ifs` lists and extracts against its manifest; verification passes every check it makes, names the two unverified fields and reports an incomplete scope; `i` prints `ifs/hbcifs` | Payload length off by one (`FSIMG_ECORRUPT`); the field at 0x10, 0x18 or a reserved byte changed (`FSIMG_EUNSUPPORTED` naming the field); a stream ending short, producing more, or with bytes after its end; a spool limit crossed at the step that crosses it with `FSIMG_RESOURCE_SPOOL`, before the host file grows past it; the image under gzip or on stdin refused naming both layers; a scheme in `ifs/mbr/hbcifs` refused naming the container |
| gzip | `gzip -t` accepts output; `zcat` yields the raw file; header bytes fixed (OS 3, flags 0, XFL by level); `gzip` and `pigz` input spools; concatenated members; MTIME clamp at an epoch of -1 and 2^32 | Trailing garbage, CRC mismatch, length mismatch, reserved flag bit, truncated member, 65 KiB FNAME; `-z` on a raw file fails as an absent signature |
| Spool | Stdin of a raw, DMG and gzip image through the coordinator's pipe; all-zero input leaves holes (physical blocks sampled); limit exactly reached; a raw stream whose prefix also passes for gzip is ambiguous and `-z` or `-F` resolves it | A gzip bomb failing at the step that crosses the limit with `FSIMG_RESOURCE_SPOOL`, before the host file grows past it; no spool descriptor; spool descriptor not a fresh empty file; a terminal on stdin refused |
| Transcode | Output bytes identical with hole knowledge consulted and ignored, across budgets and chunk-cache sizes; a sparse raw intermediate read only where its record marks writes; raw intermediate absent from every directory after success and failure; output-limit accounting of both files, in the staging directory and in the scratch tree | Short write, flush failure and cancellation at each chunk: staged files removed, nothing published, stream reports incomplete output after the first byte; a sink needing a raw intermediate without one, or with a descriptor that is linked, non-empty, not regular or read-only, fails before any mutation; `cf -` with a terminal on stdout refused |
| Chunk cache | Sequential reads of a UDZO image decode each chunk once; a pattern confined to four chunks decodes each once; decoded bytes appear in the work counter | A pattern alternating across five maximum chunks pays one decode per read and stops at `-l work=` with `FSIMG_RESOURCE_WORK`, and without a work limit at the decode bound; a 16 MiB memory budget fails the open as a memory limit |
| Wrap, unwrap, compose | Wrap then unwrap is the identity; compose of three volumes lists three partitions with the given names and types in `sgdisk`; inputs read once; a DMG input's zero run composed without a read or a decode, as the [hole design](design-holes.md#tests-and-evidence) details | Aliased input and output; five partitions on MBR; unrecognised volume without `type=` |
| Create then verify | Verification runs on the staged file before publication | A fault injected into the staged file after transcode discards it, publishes nothing, and names the layer; `cW` with `-f -` is a syntax error |
| Reproducibility | Byte-identical output across host order, budget, scratch placement, cache size and hole knowledge for every container, under `compress=none` and under zlib with one zlib version; identifiers follow the derivation under `-R` | Different epochs differ only in the derived fields; an unset identifier without `-R` fails in the library with the key named; zlib version reported |
| Fuzzing | koly, plist, mish, GPT, MBR, VHD footer, simg header, gzip header, startup header and framing, and `hbcifs` header parsers under sanitizers with small budgets | Corpus seeded from every fixture above |

Oracle availability is reported, not assumed: a missing `hdiutil`, `sgdisk`
or `simg2img` skips its gate explicitly, and the release gate requires each on
its platform.

## Open questions

* **A second spool for a spool-backed container under compression or on
  a stream.** An `hbcifs` image that is gzip-encoded or arrives on
  standard input is refused, because the one spool already holds the
  file. Lifting that needs a second handed-over spool from the coordinator
  and from every embedding application, and a second term in the spool
  accounting. Recommendation: keep the refusal; the one image that carries
  the header sits on a disc, and the refusal names both layers so a user
  decompresses first.
* **Big-endian startup headers.** `mkifs +bigendian compress=2` writes a
  byte-swapped startup header around a big-endian ImageFS, whose
  signature bytes are not this profile's identity, so such an image is
  unrecognised rather than reported as an unsupported boot image, and a
  wrap refuses a big-endian input. The options are to widen the identity
  to the swapped signature with `header_size` 256 so the claim is an
  honest "unsupported", or to read and write the swapped header as a
  second byte order of the whole profile. Recommendation: widen the
  identity to the unsupported claim; reading the swapped header is a
  profile of its own once a big-endian boot image with startup code
  exists to test it against.
