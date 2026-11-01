# Filesystem format contracts

## Format descriptors and profiles

A format descriptor is the only place that holds a format's identity:

* It names its public format or formats, its probe, independently optional
  reader and writer operations, and one capability record per public name:
  profile, entry kinds, metadata fields, geometry masks, and format and
  supported limits, including the name, label and symbolic-link target
  limits the [metadata design](design-metadata.md#capability-records)
  states.
* Capability reporting copies those records. Common lifecycle and reporting
  code never branches on a format name or variant.
* A reader's driver state is one private pointer that the descriptor's
  operations construct and destroy. Common structures name no concrete driver
  type.
* Registration is a static list. Adding a driver adds a descriptor and nothing
  in common code. This is not a plugin ABI.
* The CLI and the public header reach a capability only through a registered
  descriptor. A writer awaiting native qualification lives in a second
  descriptor that the registry does not list: the format list, detection and
  writer construction by name never see it, and tests and qualification
  tools construct writers from it through the internal
  `fsimg_writer_new_variant()`. Registering the writer
  moves its operations and write masks into the registered descriptor and
  removes the second one.

Shared helpers handle bounded I/O, endian conversion, UTF validation,
resources, and [emission](design-emission.md); a driver does not acquire host
descriptors or allocate outside its context.

The public names are `fat12`, `fat16`, `fat32`, `exfat`, `qnx6`, `qnx4`,
`ifs` and `apfs`. FAT12,
FAT16, and FAT32 share one descriptor and driver; opening selects the variant
from the validated cluster count. Detection follows one bounded probe contract:

* A probe inspects at most a descriptor-declared prefix, which common code
  reads once through the reader's session and cuts to the volume length. The
  probe is a pure function of those bytes and the volume length: it performs
  no I/O, never opens the volume, and allocates no driver state.
* A probe first tests its identity signature: fixed bytes that no other
  registered format can legitimately carry at those offsets. exFAT's is its
  jump and `EXFAT` name with a zero legacy BPB. FAT's is the jump and boot
  signature, a legal fixed BPB, and not the exFAT signature, so other boot
  records that carry a BPB, such as NTFS, are not FAT claims. Only after that
  signature matches can the probe claim the image, as a match, recognized but
  unsupported, or corrupt. Without the signature it returns no claim
  (`FSIMG_EFORMAT`).
* Automatic detection runs every probe. An I/O error or cancellation propagates
  immediately. Exactly one claim decides: a match opens that driver, while an
  unsupported or corrupt claim is reported as such. Several claims are
  ambiguous and require a forced format. No claim is an unrecognized image,
  `FSIMG_EFORMAT`.
* A forced format runs only that descriptor's probe and open, with full
  validation; an absent signature is `FSIMG_EFORMAT`. Forcing selects a
  decoder; it never skips checks or falls back to another decoder after a
  failure. A recognized format without a reader is `FSIMG_EUNSUPPORTED`.
* Recognition establishes neither supported reading, nor writing, nor complete
  validation. Those depend on the opened driver's checks and capabilities.

The default writer profile emphasizes interoperability: a 512-byte sector
default, documented legal alternative sector sizes, conservative FAT allocation
units, and explicit limits. All accepted geometry combinations are listed in
the driver's capability table and independently tested. Larger exFAT allocation
units are streamed in pieces; their size never dictates a payload-buffer size.
Format-theoretical limits and fsimg's supported profile are reported separately.

Every generated image has an entry in a fixture manifest recording format,
geometry, feature profile, source provenance, creation command, and independent
validator results. Imported fixtures and generated Unicode/codepage tables have
license/provenance records. External format tools are validation dependencies,
not code-vendoring authorization or runtime dependencies.

## FAT12, FAT16, and FAT32

Use Microsoft's FAT specification as the on-disk reference, including its
cluster-count classification rather than the volume label: fewer than 4,085
data clusters is FAT12; fewer than 65,525 is FAT16; otherwise the supported
FAT32 range applies. A regular file's size field is 32 bits, so sizes above
`UINT32_MAX` are rejected. These rules and the layout details are in the
[Microsoft FAT specification](https://www.scs.stanford.edu/~zyedidia/docs/_other/fat.pdf)
(Microsoft-authored document, hosted by Stanford).

The reader validates boot geometry, table extents, usable/reserved cluster
values, directory records, long-name sequences, and every followed chain.
FAT12 entries crossing a cache-page boundary are decoded without unaligned
loads. FAT32 uses the relevant bits and declared active/mirrored table policy;
it does not silently choose an arbitrary table when copies disagree.
Full verification checks copy consistency and allocation ownership according
to the declared policy; normal reads report their narrower scope.

The writer owns variant-specific root-directory handling, long-name slot
counts, deterministic short aliases, reserved entries, table copies, and FAT32
backup/FSInfo structures. Table generation is streamed from allocation runs.
Root capacity and directory record limits can fail independently of free data
space. Geometry near type thresholds is tested on both sides, and automatic
sizing respects the requested variant rather than changing it.

Long names preserve validated UTF-8 through UTF-16 encoding; counts are in
encoded units, not C characters. Short-name-only input uses an explicit codepage
profile, with CP437 as the supported default rather than the host locale.
Unsupported codepages fail explicitly. Case-equivalence, forbidden characters,
long-name/alias cross-collisions, and timestamp interpretation belong to the
FAT codec. Its versioned mapping tables and compatibility corpus are part of
the supported profile; no locale-dependent `toupper()` or implicit Unicode
normalization decides image names.

FAT timestamps have limited range/resolution and no universal UTC offset.
The writer uses a documented UTC-based encoding policy independent of the host
timezone; reproducible mode uses the selected epoch rounded to representable
precision. Reading exposes the original wall-clock value with unknown-zone
semantics rather than inventing a local timezone. Required unrepresentable
timestamps fail; optional rounding/loss is reported by policy.

Interoperability gates include mtools extraction and `fsck.fat` inspection,
images created/populated by independent tools, and an OS reader on isolated
test infrastructure. Tests cover all variants, fixed-root exhaustion, threshold
geometry, long-name/alias collisions, Unicode, deleted records, fragmentation,
cycles, empty files, full volumes, and the maximum file-size boundary. Host
mounting of hostile fixtures is not an ordinary developer test.

## exFAT

The normative reference is Microsoft's
[exFAT specification](https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification).
The supported profile covers ordinary non-TexFAT volumes with one FAT, normal
file/directory entry sets, allocation bitmap, upcase table, contiguous and
chained content, and 64-bit lengths constrained by the host adapter. TexFAT
and unknown critical features are unsupported, not guessed.

Readers validate both boot-region integrity and geometry, required metadata,
entry-set checksums, name hashes, UTF-16, upcase decoding, bitmap bounds, and
stream extents. `ValidDataLength` must not exceed `DataLength`; the logical
tail after valid data is returned as zero without reading stale cluster bytes.
Contiguous streams validate their complete range, and chained streams obey
bounded traversal. Root traversal does not assume the contiguous-file rule.

Writers stream allocation metadata, emit complete checksummed boot regions and
directory sets, and use a versioned deterministic upcase table. They set
contiguous-file flags only when warranted. Every cluster allocation, directory
length, name length, and checksum follows the selected profile. Timestamp
offsets are interpreted explicitly; an unknown offset remains unknown.

The upcase table is a format-bounded object, but its decoded storage still
counts against the budget. Allocation bitmaps and FATs are never mandatory
whole-volume allocations in RAM. Large zero sources can have a valid logical
length without materializing their content in memory or scratch payload files.
The writer still reserves the format-required allocation and emits its
metadata; a sparse host representation does not change ownership on disk.

Independent exfatprogs validation, externally populated volumes, and OS-reader
checks gate support. Fixtures cover >4 GiB files, initialized/zero boundaries,
fragmentation, corrupt metadata checksums, malformed entry sets, upcase/name
behavior, large allocation units, and wide directories within the format limit.
The [scale acceptance cases](architecture.md#acceptance-contract) test gigantic
logical volumes independently from large entry and fragment counts.

## QNX6 Power-Safe

The [QNX6 design](design-qnx6.md) defines classic, bootless, MMI, and extended
profiles with independent reader and writer qualification. It supplies concrete
disk layouts, snapshot and block-tree rules, bounded planning/verification,
and explicit evidence gates for unknown encodings; the [driver
design](design-qnx6-driver.md) defines the code that implements the classic
profile. Unknown compatibility
markers are unsupported; a valid magic or Linux mount is insufficient.

The writer constructs a new volume with complete snapshot and allocation
metadata suitable for native QNX use. Native write-after-create testing gates
the claim, independently of read-only interoperability. Source-informed
research imports no GPL code. Porsche HDD compatibility also requires exact
disk composition and any other filesystem present, particularly QNX4; it is
not implied by a `qnx6` format capability.

## QNX4

The [QNX4 design](design-qnx4.md) defines the 512-byte-block, extent-based
filesystem that `fs-qnx4.so` mounts: geometry derived from the slice length,
the root block, the 64-byte inode and link records, extent-block chains, the
bitmap, `.inodes`, `.longfilenames` and the hidden format-owned files. Its
acceptance tables are closed by native QNX 6.5.0 observation and the PCM 3.1
navigation partition; an unobserved value is unsupported, a contradiction is
corrupt. The [driver design](design-qnx4-driver.md) defines the code. QNX4
has no magic number: the probe's identity signature is the root record of
block 2, and `open` completes recognition.

The writer reproduces the native formatter's default layout with one
contiguous extent per object. Native check, mount, modify and remount
testing gates the writer capability, as for QNX6. Long names, device nodes
and the head unit's own filesystem module have their own evidence gates in
the [qualification work](../docs/todo-qnx4.md).

## QNX ImageFS

The [ImageFS design](design-ifs.md) defines the QNX image filesystem that
`mkifs` builds and `procnto` mounts from a boot image: a fixed header
identified by `imagefs` at offset 0, a mountpoint, a table of records that
each carry a full relative path, file data, and a four-byte additive
checksum. There is no allocation structure and no directory storage; the
hierarchy is derived from the sorted paths, missing parents are
synthesized, and a path stored twice has one entry, the last record, with
the earlier ones counted as shadowed. Its acceptance tables are closed by
native QNX 6.5.0 observation and by the PCM 3.1 and live-CD boot bodies:
little- and big-endian images, four record kinds, one symbolic-link layout,
four inode-flag values; an unobserved value is unsupported, a contradiction
is corrupt. The [driver design](design-ifs-driver.md) defines the code. The
driver reads a bare ImageFS; the startup header, boot prefix and compressed
chunking of a boot image belong to the `ifsboot` container and the
Harman-Becker header to the `hbcifs` container, both
[layer profiles](design-layers.md#layer-profiles).

The writer builds a standalone ImageFS in one deterministic layout whose
size is exact, so the format has no free space and no geometry to size,
and it is [nonbootable](design-ifs.md#bootable-bodies): a bootable body
comes from `mkifs` or an existing boot image. Native `dumpifs` list,
check and extract testing gates the writer capability, as native mounting
does for QNX6 and QNX4; wrapping a bootable body, built by `mkifs` or
unwrapped from an existing boot image, is the `ifsboot` container's, gated
by a native boot.

## APFS

The [APFS design](design-apfs.md) defines the Apple File System as an APFS
container holding volumes: the container superblock and its checkpoint
rings, ephemeral, physical and virtual objects with Fletcher-64 checksums,
object maps keyed by object and transaction, one B-tree format, volume
superblocks with closed feature tables, filesystem records, hashed directory
names compared under Unicode normalization and case folding, sparse and
cloned data streams, transparent compression, snapshots and sealed volumes.
The probe's identity signature is the `NXSB` magic with the superblock's
object kind and identifier; `open` selects the newest complete checkpoint
and one volume, chosen by the `volume=` read setting when the container
holds several, and a snapshot by `snapshot=`. Encryption and Fusion are
recognized and unsupported. The [driver design](design-apfs-driver.md)
defines the code.

The writer builds a new container with one unencrypted volume, reproducing
the native formatter's empty layout and adding the catalog in one
transaction. Native `fsck_apfs`, mount and write-after-create testing on
macOS gates the writer capability, as native testing does for the QNX
writers; the Linux checkers are a second opinion, never a substitute.

## Verification and compatibility claims

Every registered capability has a corresponding
[driver contract-test profile](design-driver-contract-tests.md). Those tests
exercise common API, lifecycle, resource and fault behavior with independently
specified expectations. They supplement each format's malformed-image cases
and native qualification; a shared round trip does not establish compatibility.
The [verification helpers](design-verification-helpers.md) consolidate payload
and ownership-run mechanics while leaving format-specific verification here
and in each driver design.

Info (`i`) describes recognized geometry and implemented capabilities.
Verification (`W`) performs complete structural checks for the supported
profile and optionally reads payloads. Neither performs filesystem repair or
claims that arbitrary unknown extensions are safe to ignore. Unknown benign
records are skipped only where the format expressly defines that behavior and
their lengths can be validated.

A driver's read-only implementation does not need writer stubs. Adding a
filesystem requires a design that accounts for geometry, feature negotiation,
ownership, names, traversal termination, memory/scratch complexity, hostile
inputs, and independent validation. NTFS requires a separate reader-first
design and qualification gates. QNX6 and QNX4 reading and writing follow their
own designs; no broad compatibility claim is made merely from parsing a boot
record.
