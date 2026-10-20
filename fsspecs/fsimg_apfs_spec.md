# APFS on-disk format and fsimg implementation specification

**Edition:** 2026-10-19 · **Status:** implementation design and research specification, not an official Apple specification · **Target:** `fsimg` read/extract and image creation · **Source discipline:** independent text, original algorithms, public format facts only; no verbatim non-BSD source code.

> **Scope and confidence.** The canonical public on-disk description is [Apple File System Reference (2020-06-22)](https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf), 181 pages. Its structure names and numeric layout facts are used here for interoperability, but its prose, C declarations and code are not reproduced. Recent Apple OS releases can emit constructs not covered by the 2020 document. The implementation MUST feature-gate unsupported layouts and maintain Apple-generated regression images for newer versions. A BSD-3-Clause Go implementation published in 2026 is another promising comparison point, but its native interoperability and encryption claims are repository claims, not independently established facts in this document. [go-filesystems/apfs](https://github.com/go-filesystems/apfs).

This document is deliberately an **engineering specification for an image library**, not a kernel filesystem driver specification. It prioritizes deterministic extraction, reproducible image generation, validation, crash consistency and meaningful error reporting. It distinguishes *known layout*, *recommended policy*, *requires test vectors*, and *deferred functionality*.

## 0. Executive decision and implementation tiers

**Recommended result:** implement **read-only, unencrypted, single-store APFS volume extraction** first. An APFS writer is practical but should be a separate module whose initial deliverable is a clean, independently mountable *new image*; don't build general in-place mutation into the fsimg API just to support APFS.

| Tier | Behavior | Priority | Acceptance oracle |
|---|---|---|---|
| R0 | Probe raw/GPT/APFS; read NXSB and selected checkpoint; verify checksums; list volumes | P0 | macOS `diskutil apfs list`, raw hex inspector |
| R1 | Object maps, multi-level B-trees, FS records; list directories, symlinks, extract ordinary files | P0 | macOS mounts, `fsck_apfs -n`, `cmp`, `find` |
| R2 | xattrs, hard links, sparse files, clones, case folding, volume roles, shared-container volumes | P1 | macOS `ls`, `stat`, `xattr`, file hashes |
| R3 | Snapshots, compressed files, sealed System volume **read** and named stream handling | P2 | macOS snapshot mounts, reference decoders |
| R4 | Software-encrypted volumes, Fusion, recovery from incomplete transactions | P3 | dedicated fixtures, reference libraries |
| W0 | Create *new*, unencrypted, 4 KiB-block, single-store APFS container with one volume and basic files | P1/P2 | native `hdiutil attach`; clean `fsck_apfs -n` |
| W1 | Multiple volumes, xattrs, hardlinks, sparse files, atomic finished-image commit | P2 | native macOS write/read after fsimg creation |
| W2 | Clone/snapshot/copy-on-write writer, extent sharing, compressed writer, resize | P3 | allocator/refcount/snapshot stress |
| W3 | Encryption, sealed writer, bootable macOS volume group, multi-device Fusion | Explicitly deferred | substantial extra protocols |

### 0.1 What APFS is and isn't

An APFS **container** is a block-addressed allocation domain; it holds an object map, a spaceman allocator, transactional checkpoints, and up to a bounded collection of APFS volumes. Each **volume** has its own metadata tree, object map, namespace, attributes and snapshots; sibling volumes share container free space. An APFS container may occupy a raw device, a GPT Apple_APFS partition, or a physical store inside a virtual disk or DMG. **UDIF/DMG, GPT and APFS are distinct formats**: do not interpret a `.dmg` header or GPT block zero as an APFS container block zero.

Metadata updates use **copy-on-write object versions**, keyed by transaction identifiers (XIDs). Resolving a virtual object means asking for the newest mapped physical version not newer than the chosen XID. The most recent valid committed checkpoint governs mount state. Merely reading block zero does not necessarily select the active image state.

### 0.2 Core diagram

```text
physical file / block device
  ├── optional UDIF / sparsebundle / other disk-image translation
  ├── optional GPT: Apple_APFS partition (container byte origin)
  └── APFS container (physical block addresses relative to origin)
      ├── block 0: nx_superblock (NXSB)
      ├── checkpoint descriptor region: checkpoint maps + NXSB versions
      ├── checkpoint data region: ephemeral objects
      ├── container object map (OMAP): virtual oid + xid -> paddr
      ├── spaceman, allocation bitmaps, free-space queues
      ├── optional keylocker / Fusion / jumpstart
      └── nx_fs_oid[]: volume superblock virtual identifiers
          ├── volume APSB -> volume object map
          │    ├── FS tree (inodes, dir records, xattrs, file extents)
          │    ├── extent-reference tree
          │    ├── snapshot metadata tree
          │    └── sealed-volume file-extent / integrity structures
          └── additional volumes ...
```

**Do not assume** that an APFS container has exactly one volume, that a volume starts at any fixed disk offset, or that all its B-tree blocks belong to a contiguous extent.

## 1. Terminology, bytes and invariants

### 1.1 Integer and addressing conventions

* All multibyte integers shown below are **little-endian**, except where explicitly defined as byte arrays, raw UTF-8 or UUID bytes. All decoded offsets/lengths must be checked before indexing an input buffer.
* `paddr_t`: a **signed 64-bit physical block number**, relative to the start of the APFS container physical store. A physical read at `paddr` uses `store_base + paddr * nx_block_size`; a negative or overflowed address is invalid unless a *specific* structure gives it a non-address sentinel meaning.
* `oid_t`: 64-bit object identifier, *not necessarily an on-disk block number*.
* `xid_t`: 64-bit monotonically increasing transaction identifier used for object version selection.
* `prange_t`: physical range represented by starting `paddr` and block count.
* A **filesystem object** is typed metadata (not merely a file). An **inode** is a file/directory metadata record in the FS tree.
* In a filesystem-tree key, the most significant four bits identify the record kind; the other **60 bits** identify the owning object. Owner 2 is the normal root directory inode.
* APFS timestamps use **signed/unsigned 64-bit nanoseconds relative to 1970-01-01 UTC**, as appropriate for the field, rather than HFS+ 1904 seconds. When mapping to host `time_t`, clamp/check overflow and preserve nanoseconds.
* APFS's default allocation block is **4096 bytes**; container block size is available on disk and should not be assumed immutable. Apple's published limits span 4 KiB through 64 KiB, subject to other format constraints. Container minimum is approximately 1 MiB; tiny-container practical restrictions need native testing.
* The UUIDs and hashes are binary on disk. Endianness of RFC 4122-formatted UUID display should be handled deliberately, not by a host-endian cast.

**Reference:** Apple, *General-Purpose Types*, *Objects* (pp. 9–21), *Container* (pp. 26–40).

### 1.2 Outer-image probing

| Image class | Detect | Next action | fsimg scope |
|---|---|---|---|
| Raw APFS container | valid NXSB at image-relative byte `0` | `base=0` | mandatory |
| GPT disk | GPT header at sector LBA1 and valid partition entries | locate partition type `7C3457EF-0000-11AA-AA11-00306543ECAC`; use partition start as base | mandatory |
| Apple UDIF read/write DMG | valid trailer/UDIF descriptor (may be compressed/sparse) | present logical device view before parsing GPT or NXSB | **separate backend**, not required for APFS itself |
| Sparsebundle | bands described by image metadata | logical device adapter | separate backend |
| Fusion | APFS single logical container distributed across two stores | special address routing/validation | deferred |
| FileVault encrypted image | same superblocks, protected metadata/data | keys and crypto required | detect & report |

`fsimg probe` should return `APFS` only after magic **plus** plausible block size/count, checksum, physical bounds, and version-feature checks. A four-byte `NXSB` match alone is not adequate evidence.

### 1.3 Generic metadata-object prefix

Most ordinary APFS metadata blocks start with a 32-byte object header:

| Byte offset | Size | Meaning |
|---:|---:|---|
| `0` | 8 | block checksum (`o_cksum`) |
| `8` | 8 | object ID (`o_oid`) |
| `16` | 8 | transaction ID (`o_xid`) |
| `24` | 4 | object type/flags (`o_type`) |
| `28` | 4 | subtype (`o_subtype`) |

Type codes occupy the low 16 bits of `o_type`; higher bits express storage and format flags. Common base object kinds include NX superblock, B-tree root/node, spaceman, spaceman chunk-info address blocks, chunk-info blocks, checkpoint map, object map, APFS volume superblock, fs tree, extent-reference tree, snapshot metadata, reaper and keybag structures. **Use a typed registry**, not a universal assumption that every block contains the generic header. Some data blocks and hashed/no-header B-tree variants do not.

**Three classes of object ID are fundamental:**

1. **Physical:** OID identifies physical location; read that location directly (subject to correct store routing).
2. **Virtual:** OID resolves through the relevant volume/container object map at chosen XID.
3. **Ephemeral:** OID resolves through the **selected checkpoint's mapping**, generally pointing to checkpoint data blocks. Ephemeral objects include allocator/control structures.

A read must know `object_kind`, `omap_scope`, `xid`, and (when applicable) the selected checkpoint. Using a single global `oid -> paddr` hash map without version/scope is incorrect.

### 1.4 Fletcher-64 checksums

Most metadata objects carry a Fletcher-64-like two-sum checksum over their **entire object block except the first eight checksum bytes**, viewing that remainder as little-endian `uint32` words. Modulus is `m=2^32−1`. The check value is composed of two 32-bit correction words chosen so the sums over the *whole block including checksum*, interpreted in checksum-first storage order, satisfy APFS's validation relationship.

For fresh block generation, a practical independently derived procedure is:

```text
s1 = 0; s2 = 0
for each little-endian u32 word of bytes [8, block_size):
    s1 = (s1 + word) mod (2^32 - 1)
    s2 = (s2 + s1) mod (2^32 - 1)
lo = m - ((s1 + s2) mod m)
hi = m - ((s1 + lo) mod m)
store_u64_le(block[0:8], (hi << 32) | lo)
```

This is **new pseudocode**, not copied from an implementation. Test it against Apple-generated NXSB/BTREE fixture blocks before trusting it. Do not checksum ordinary file payload as if it were an object. When a format says a B-tree has no object header, validate it according to its own integrity scheme.

### 1.5 Reader-wide safety invariants

* Never multiply `block_count * block_size`, add offsets, or convert a 64-bit signed paddr to `size_t` without checked arithmetic.
* Each metadata read declares a required minimum length and the allowed containing allocation/object size; partial images fail explicitly.
* Check object IDs, types/subtypes and XIDs when the format requires matching identity. Some types legitimately embed variable-sized payloads or have no header.
* Restrict B-tree height, fanout, number of total visited nodes, directory depth, symbolic-link resolution depth, xattr size and compression output by configurable budgets.
* Treat **holes as zero bytes** in stream reads but **not** as permission to follow `paddr=0` metadata mappings.
* Preserve unknown but safely delimited fields in diagnostics; block writes when semantics are unknown.
* Differentiate `CORRUPT_IMAGE`, `UNSUPPORTED_FEATURE`, `KEY_REQUIRED`, `IO_ERROR`, `NOT_FOUND` and `INVALID_ARGUMENT`.

## 2. Container superblocks and transaction checkpoints

### 2.1 Primary container block

A raw APFS container starts with an `nx_superblock` at physical block 0. An object header occupies its first 32 bytes; magic `NXSB` has little-endian integer representation **`0x4253584e`** in the next four bytes. A subset of important fields, **offsets relative to beginning of the physical object**, is:

| Offset | Width | Field / interpretation |
|---:|---:|---|
| `0` | 32 | generic object header |
| `32` | 4 | magic `NXSB` |
| `36` | 4 | APFS block size in bytes |
| `40` | 8 | physical block count |
| `48` | 8 | optional-feature bitmap |
| `56` | 8 | read-only-compatible-feature bitmap |
| `64` | 8 | incompatible-feature bitmap |
| `72` | 16 | container UUID |
| `88` | 8 | next allocatable object ID |
| `96` | 8 | next transaction ID |
| `104` | 4 | checkpoint descriptor area block count / encoding |
| `108` | 4 | checkpoint data area block count / encoding |
| `112` | 8 | checkpoint descriptor area base block |
| `120` | 8 | checkpoint data area base block |
| `128`, `132` | 4 each | next insertion position for descriptors/data |
| `136`, `140` | 4 each | descriptor ring active start index and length |
| `144`, `148` | 4 each | data ring active start index and length |
| `152` | 8 | ephemeral spaceman OID |
| `160` | 8 | physical container OMAP OID |
| `168` | 8 | reaper OID |
| `176` | 4 | diagnostic/test type |
| `180` | 4 | maximum volume-superblock slots |
| `184` | `100×8` | array of 100 virtual volume-superblock OIDs (unused slots zero) |

A later region contains counters, blocked ranges, EFI jumpstart, Fusion and keylocker references, named metadata, and alignment/reserved data. Parse later fields only when their layout/feature prerequisite is known; **no writer may memcpy a truncated NXSB prototype**. Inspect the complete Apple superblock section when implementing W0.

The `nx_max_file_systems` constraint is additionally bounded by available container space; Apple documents a limit based on 512 MiB per allowed volume slot (and an absolute 100 slots). This is *not* a statement that every volume reserves 512 MiB.

**Reference:** Apple *Container* pp. 26–40, plus *Partition UUIDs* p. 25.

### 2.2 Container feature-mask handling

Three unrelated bitmaps have intentionally different semantics:

* **Optional features:** an unaware reader may not need to interpret every enabled optimization, but must still understand the structures it actually encounters.
* **Read-only compatible:** unknown bits block a **writer**, even when the reader can continue.
* **Incompatible:** unknown bits block both read and write unless explicitly supported.

Known container incompatible features in Apple's public reference include version flags and Fusion support. Examples: `NX_INCOMPAT_VERSION1` (`0x1`), `NX_INCOMPAT_VERSION2` (`0x2`), and `NX_INCOMPAT_FUSION` (`0x100`). Version 1 relates to an old development format; modern APFS typically uses version 2. **Treat Fusion as unsupported until two-store address handling exists.** Feature interpretation should be based on a maintained table tied to a spec revision, not a blanket refusal of every modern optional bit.

### 2.3 Descriptor and data rings

APFS reserves two kinds of checkpoint space:

* The **descriptor area** contains candidate container superblocks and checkpoint mapping objects. The region may be a contiguous range or use an indirect descriptor scheme; a high bit in the block-count encoding is meaningful. Don't silently ignore that encoding.
* The **data area** holds ephemeral objects referenced by checkpoint mappings. Ring indexes/lengths and `next` positions roll modulo the respective area's declared capacity; ranges must be consistent with the physical store.

Checkpoint entries contain an object header and a count of mappings. A mapping names an ephemeral OID, the object type/subtype, byte length, owning filesystem where applicable, and the actual physical storage block. The final mapping object in a checkpoint is identified with `CHECKPOINT_MAP_LAST` (`0x1`). A checkpoint is a **consistent collection**, not an arbitrary list of pointers picked from multiple transaction IDs.

A naive parser that chooses the highest-XID NXSB seen in any block can incorrectly select an uncommitted or damaged transaction. The selection algorithm must check **all relevant checkpoint structural conditions**, then prefer the highest complete, valid XID. If that fails, roll back to the next fully valid checkpoint, subject to a caller policy (`strict` versus `recover`).

### 2.4 Original checkpoint-selection algorithm

```text
select_checkpoint(store):
  b0 = read_raw_block(0, enough_bytes_for_nxsb_prefix)
  check magic, checksum, block_size, block_count, region arithmetic
  candidates = {b0}
  enumerate descriptor ring using b0's bounds and indirect-region rules
  for each descriptor slot:
      if the slot could contain NXSB or checkpoint-map:
          independently verify its checksum and payload bounds
          add eligible NXSB candidates
  sort candidates by descending transaction ID
  for nx in candidates:
      if flags/layout supported and checkpoint map set is complete:
          if every mandatory ephemeral object is mapped consistently:
              if container OMAP root and volume array can be resolved:
                  return checkpoint(nx.xid, nx, ephemeral_map)
  return CORRUPT_IMAGE or UNSUPPORTED_FEATURE with reason
```

**Implementation notes:** `nx_xp_desc_base` and descriptor-ring size used to find candidates must themselves be treated as untrusted. To avoid deriving availability solely from a damaged block-zero NXSB, a recovery mode can bounded-scan plausible metadata positions after reading a sane block size; do not implement unlimited scanning by default. A reader may optimize by caching ephemeral mappings for the selected checkpoint, but must not merge mappings from distinct XIDs.

### 2.5 Checkpoint map record layout

Each mapping record occupies 40 bytes of logical fields, consisting of two 32-bit type identifiers, a 32-bit object byte size, a 32-bit reserved field, and three 64-bit numbers: filesystem owner ID, ephemeral object ID, physical address. Preserve the distinction between mapped **object byte size** and **allocation block count**. The checkpoint-map object itself begins with the generic object header and has a 32-bit flags field and a 32-bit mapping count before the array; validate count against usable block bytes.

**Reference:** Apple *checkpoint_mapping_t*, *checkpoint_map_phys_t*, *Checkpoint Flags* pp. 40–42.

## 3. Object-map resolution

The container has an OMAP; each volume generally has its own OMAP. These mappings are key to understanding APFS's virtual object IDs and snapshots.

### 3.1 OMAP header and keys

The object-map physical object has the common 32-byte object header followed by a set of fields that identify its B-tree root, snapshot-tree root, flags and transaction limits. It is itself a **physical** object. Its main index B-tree is a mapping from `(objectID, transactionID)` to `(physicalAddress, size, flags)`.

| On-disk unit | Field sequence | Size |
|---|---|---:|
| OMAP key | `oid` (u64), `xid` (u64) | 16 B |
| OMAP value | flags (u32), object size (u32), paddr (signed 64) | 16 B |
| Important OMAP flags | `DELETED=1`, `SAVED=2`, `ENCRYPTED=4`, `NOHEADER=8`, `CRYPTO_GENERATION=16` | bit values |

For a lookup at XID `T`, consider keys with the exact OID and transaction ID `≤ T`, and choose the **greatest such transaction ID**. A deletion tombstone at the selected version means *not found* at that XID; it is not legitimate to fall through to an older mapping. Physical addresses in OMAP records are validated in the correct physical store.

### 3.2 Resolution example

```text
Volume OMAP entries:
  (oid=701, xid=100) -> paddr 45
  (oid=701, xid=114) -> paddr 72
  (oid=701, xid=121) -> deleted

lookup(701, snapshot_xid=110) = block 45
lookup(701, snapshot_xid=120) = block 72
lookup(701, snapshot_xid=123) = NOT_FOUND
```

This is a synthetic example, not a dump of any reference code.

### 3.3 Original lookup algorithm

```text
resolve_virtual(omap, wanted_oid, visible_xid):
    key = predecessor_or_equal((wanted_oid, visible_xid), omap.tree)
    if key doesn't exist or key.oid != wanted_oid: NOT_FOUND
    val = decode_omap_value(key.value)
    if val is tombstone: NOT_FOUND
    validate size, storage class, block boundaries
    read object bytes via val.paddr
    if NOHEADER is not set:
        verify checksum and expected oid / xid / type
    return object
```

**Object maps need a lexicographic numeric `(oid,xid)` comparator**, not bytewise comparison of the little-endian serialized key. Do not confuse the selected NXSB XID with the newer possible OMAP entries. The physical OMAP tree's nodes may themselves have physical addressing; the B-tree layer must support physical or virtual child resolution according to tree flags. Snapshot OMAP trees add another level of historical metadata.

### 3.4 Volume discovery

1. From selected NXSB, open `nx_omap_oid` as the container OMAP.
2. For each nonzero `nx_fs_oid[i]`, resolve the volume's virtual superblock OID at checkpoint XID through the container OMAP.
3. Validate the volume object's `APSB` magic and compatible feature masks.
4. Open volume OMAP using `apfs_omap_oid`, normally a physical OID.
5. Resolve root FS-tree OID via **volume** OMAP at chosen XID; enumerate names and inodes from that tree.
6. Never assume volume array order matches `diskutil` synthesis order or system boot role.

**Reference:** Apple *Object Maps* pp. 44–50; *Mounting an Apple File System Partition* pp. 26–27.

## 4. Shared B-tree engine

Almost everything interesting in APFS is found in B-trees: OMAP entries, filesystem records, extent references, snapshot records, free-space queues and optional sealed-volume data. Build one defensive B-tree walker parametrized by tree key/value codecs, comparators and object-resolution policy.

### 4.1 Node header and logical geometry

A typical B-tree node uses a generic 32-byte object header. The next 24 bytes describe the node, beginning at byte 32:

| Offset | Width | Meaning |
|---:|---:|---|
| 32 | 2 | node flags |
| 34 | 2 | level; leaf is level 0 |
| 36 | 4 | number of entries |
| 40 | 4 | TOC location (`nloc`: u16 offset, u16 length) |
| 44 | 4 | free-space location |
| 48 | 4 | free-list location for key space |
| 52 | 4 | free-list location for value space |
| 56 | varies | node data region, with TOC, keys and values |

Root nodes have a **40-byte `btree_info` footer**, composed of a fixed 16-byte parameter segment plus statistics including longest key/value lengths, total key count and node count. Compute the footer's actual position from the node's documented root/fixed-size rules; **root usable value space ends before the footer**. Some modern sealed-tree nodes can be hashed or no-header nodes. The node's `BTN_ROOT`, `BTN_LEAF`, `BTN_FIXED_KV_SIZE`, `BTN_HASHED`, `BTN_NOHEADER` behavior matters.

Useful logical flags include `BTREE_ALLOW_GHOSTS`, `BTREE_EPHEMERAL`, `BTREE_PHYSICAL`, `BTREE_KV_NONALIGNED`, `BTREE_HASHED`, `BTREE_NOHEADER`. Avoid interpreting node flags as if they were the exact same namespace as persistent tree-info flags.

### 4.2 Table of contents and offsets

Two TOC layouts exist:

* **Variable width:** each TOC item is two `(offset:u16, length:u16)` pairs (8 bytes total). The first locates a key, the second a value.
* **Fixed key and value sizes:** each item is two 16-bit offsets (4 bytes total); lengths come from the root's fixed-size tree metadata.

TOC entries occupy space identified by `btn_table_space` relative to the start of the node data. Key payload grows forward from the end of the TOC; value payload is addressed relative to the *back* of the usable node data (or footer boundary), with the documented negative-offset convention. **This is the major offset-arithmetic trap.** Entry offsets are not universal absolute byte offsets into the block.

For every key/value pair, validate:

1. TOC size = `nkeys × entry_size` and fits the declared TOC region.
2. Decoded key range does not exceed key region; decoded value range does not hit the root footer.
3. Ghost entries (`0xffff` invalid value offset) only appear when allowed and must not be treated as arbitrary memory.
4. Keys are monotonically ordered under the comparator for this *particular tree*.
5. Internal-node levels decrease correctly and there are no cycles (including repeated physical blocks from corrupt references).
6. Key size and value size are reasonable; do not trust `bt_longest_key` or `bt_longest_val` as allocation instructions.

### 4.3 Inner vs leaf nodes

Leaves contain actual records. Inner nodes index lower-level blocks; their values contain a child OID (u64). Hashed internal nodes can carry an additional 64-byte slot for a child cryptographic digest. Resolving that child OID depends on the tree's physical/virtual addressing mode. The entire child object can be part of a sealed-tree hash chain, but verifying Merkle integrity is independent of the ability to follow the pointer.

**Implementation warning:** a sealed tree with node digests may be *traversable without authenticating the digests*. Report `integrity_verified=false` unless cryptographic verification against a trusted root/seal is actually implemented.

### 4.4 Comparator families

| Tree | Correct ordering domain |
|---|---|
| Object map | numeric `(oid, xid)` |
| Filesystem | numeric packed `(ownerID, recordType)` with record-specific suffix comparison |
| Directory entries | parent owner, record type, then *documented name/hash representation*; respect normalization/case rules |
| File extents | stream owner, then numeric logical byte address |
| Xattrs | owner ID and attribute-name bytes with key encoding |
| Snapshot names | snapshot-name key and string rules |
| Free-space queue | transactional number and physical-address ordering |

Do not globally compare serialized keys with `memcmp()`. It will fail for numerically ordered little-endian integers, and filename comparisons require normalization semantics. A B-tree implementation can expose `seek_ge(key)`, `seek_le(key)`, `iterate_prefix(key)`, `iterate_range(...)`, `scan_all()` as reusable primitives.

### 4.5 Walker sketch

```text
read_tree_node(oid, expected_level):
  block = resolve(tree.address_class, oid, read_xid)
  validate node envelope, footer, flags and TOC
  entries = decode_toc_bounded(block)
  assert sorted(entries, tree.compare)
  if leaf: return records
  for each entry in range of interest:
      child = value.child_oid
      child_node = read_tree_node(child, expected_level - 1)
      combine child results
```

Avoid recursive stack overflow on corrupt images by using an explicit stack and a visited-set. For encrypted/no-header/hardware-key dependent nodes, return `UNSUPPORTED_FEATURE`/`KEY_REQUIRED` instead of inventing zeros.

**Reference:** Apple *B-Trees* pp. 122–134, *Sealed Volumes* pp. 149–161.

## 5. APFS volume superblock and feature model

Each volume's `apfs_superblock` is found through the **container** OMAP. Its first 32 bytes are the generic object header; its magic at byte 32 is the four ASCII bytes `APSB` (little-endian integer representation `0x42535041`). Important fields in the documented structure occur in this sequence:

| Group | Data | fsimg use |
|---|---|---|
| Identification | APSB magic, filesystem slot index, volume UUID, 256-byte volume-name array | enumerate and label volumes |
| Compatibility | optional, read-only-compatible and incompatible feature masks | decide supported read/write features |
| Capacity | reserve count, quota count, allocated blocks | display allocation information; enforce W0 quotas if set |
| Metadata roots | OMAP OID, FS-tree OID, extent-reference OID, snapshot-metadata OID, respective object-type fields | build tree handles |
| Transactions | reverting XID/OID, last unmount time, next object ID, most recent modification | choose historic namespace safely; allocate IDs in writer |
| File counts | regular files, directories, symlinks, other records, snapshots, allocation/freed totals | cross-check enumeration, not authoritative stream lengths |
| Crypto | metadata crypto state and filesystem crypto flags | decide if a key is required |
| macOS roles | 16-bit role, volume-group UUID, System/Data split metadata | surface all volumes separately, optionally form union view |
| Integrity | sealed-volume integrity metadata OID, optional file-extent tree and type | sealed-mode read and verification |

The volume superblock is a transaction-versioned **virtual object**. A snapshot can reference older volume-superblock and object-map versions. The in-memory handle should include the selected checkpoint and XID.

### 5.1 Volume identity and role

Volume roles (not a bitset of concurrent roles) include: `NONE=0`, `SYSTEM=0x1`, `USER=0x2`, `RECOVERY=0x4`, `VM=0x8`, `PREBOOT=0x10`, `INSTALLER=0x20`, `DATA=0x40`, `BASEBAND=0x80`, `UPDATE=0xc0`, `XART=0x100`, `HARDWARE=0x140`, `BACKUP=0x180`, `ENTERPRISE=0x240`, `PRELOGIN=0x2c0`. Newer operating systems may introduce additional values. Do not treat a role as a mount-path instruction; an APFS backup image can be useful even when no host OS uses that role.

**System/Data volume groups.** Since Catalina, user-visible `/` may be assembled from a sealed or read-only System volume and a separate mutable Data volume with the same `apfs_volume_group_id`. Synthetic firmlink presentation is performed by the OS. For fsimg, the least surprising default is **volume-separated extraction** (`volume/System/...`, `volume/Data/...`) rather than trying to merge them invisibly. Expose a separate `--apfs-volume-group=unified` mode later, with explicit firmlink handling and collisions.

### 5.2 Important volume flags and compatibility policy

| Flag or bit | Value | Behavior |
|---|---:|---|
| `APFS_FS_UNENCRYPTED` | `0x1` | unencrypted volume flag |
| `APFS_FS_ONEKEY` | `0x8` | volume-encryption-key mode, typically macOS FileVault |
| `APFS_FEATURE_HARDLINK_MAP_RECORDS` | `0x2` | sibling mapping records are present |
| `APFS_FEATURE_STRICTATIME` | `0x8` | more aggressive file-access timestamp updates |
| `APFS_FEATURE_VOLGRP_SYSTEM_INO_SPACE` | `0x10` | shared System/Data inode-number arrangement |
| `APFS_INCOMPAT_CASE_INSENSITIVE` | `0x1` | case-insensitive namespace |
| `APFS_INCOMPAT_DATALESS_SNAPS` | `0x2` | data-less snapshots may exist |
| `APFS_INCOMPAT_ENC_ROLLED` | `0x4` | encryption keys have been rotated |
| `APFS_INCOMPAT_NORMALIZATION_INSENSITIVE` | `0x8` | canonical-equivalent names match |
| `APFS_INCOMPAT_INCOMPLETE_RESTORE` | `0x10` | interrupted restore; **do not write** |
| `APFS_INCOMPAT_SEALED_VOLUME` | `0x20` | immutable/sealed volume; no writing |

Volume-level optional features, read-only-compatible flags and incompatible flags are independent masks. A writer should support a deliberately **narrow profile** rather than copy arbitrary flags from an existing image. Reserved flags are not instructions to preserve their state when creating a new image; leave them clear, and preserve them only in a future full modification implementation where explicitly permitted.

**Reference:** Apple *Volumes* pp. 51–70 (roles pp. 63–66, compatibility pp. 67–69).

## 6. Filesystem tree record encoding

### 6.1 FS tree key family

APFS's namespace and stream records share one filesystem-tree key space. Every key begins with a 64-bit packed word:

```text
kind = packed_key >> 60
owner = packed_key & 0x0fff_ffff_ffff_ffff
```

The **kind** says how to decode the remaining key bytes and the value. Key fields after the leading word depend on kind: record names, logical offsets, sibling IDs, etc. The same numeric object ID can have multiple logically separate record kinds. The FS tree must be addressed through the **volume OMAP** unless the tree type says otherwise.

| Kind nibble | Public APFS record | Key suffix | Typical value |
|---:|---|---|---|
| 0 | any / invalid sentinel | – | never ordinary FS record |
| 1 | snapshot metadata | none | snapshot state |
| 2 | physical extent | none | physical length, owner, refcount |
| 3 | inode | none | permissions, owner, times, xfields |
| 4 | extended attribute | NUL-terminated name length + bytes | embedded bytes or stream reference |
| 5 | sibling link | sibling ID | parent and name for hardlink |
| 6 | data-stream ID | none | stream reference count |
| 7 | crypto state | layout-specific | per-stream protection state |
| 8 | file extent | logical byte address | physical block and run length |
| 9 | directory record | name or hashed name | inode ID, type and add time |
| 10 | directory stats | none | child and size counters |
| 11 | snapshot name | snapshot name string | snapshot XID |
| 12 | sibling map | none | underlying inode ID |
| 13 | file info | content/hash key suffix | sealed-file data hash |
| 14–15 | reserved / undefined in 2020 reference | unknown | reject as unknown |

The keys are not all lexically comparable by their serialized bytes. A specific comparator also needs to correctly handle same-owner records of different kinds and the entry-dependent string/hash components.

### 6.2 Reserved inode numbers

The normal root directory is **inode 2**. Inode 1 is a parent-of-root sentinel; inode 3 is the `private-dir` used internally by APFS; Apple reserves a small set of additional special identifiers, including 6 and 7 for directory-related machinery. IDs below 16 are reserved; ordinary newly created objects use IDs at or above 16, except where the volume group divides the ID space. The Apple document includes additional constants for special objects; consult the *Inode Numbers* section rather than replacing all low IDs with regular files.

**Writer acceptance warning:** even a seemingly empty volume must have the native-required root and private metadata records. A filesystem that merely has a valid NXSB and a `root` directory entry is **not necessarily mountable by macOS**. Capture the exact ordinary, unencrypted `newfs_apfs` initialization profile through fixtures before attempting W0.

### 6.3 Inode value: fixed 92-byte prefix

The packed inode value's fixed portion is 92 bytes, optionally followed by an extended-field blob. Byte offsets in this table are **relative to the value beginning**, not to the B-tree block:

| Value offset | Width | Property |
|---:|---:|---|
| 0 | 8 | primary parent inode ID |
| 8 | 8 | private ID identifying the data stream |
| 16 | 8 | creation timestamp, nanoseconds since Unix epoch |
| 24 | 8 | data modification timestamp |
| 32 | 8 | inode metadata change timestamp |
| 40 | 8 | last access timestamp |
| 48 | 8 | APFS internal inode flags |
| 56 | 4 | child count **if directory**, or hardlink count **otherwise** |
| 60 | 4 | protection-class ID |
| 64 | 4 | write-generation counter |
| 68 | 4 | BSD flags (`chflags` semantics) |
| 72 | 4 | UID |
| 76 | 4 | GID |
| 80 | 2 | POSIX mode (file kind and permissions) |
| 82 | 2 | padding/reserved |
| 84 | 8 | uncompressed size (especially for compressed streams) |
| 92 | variable | extended-field block, if present |

`private_id` is critical: the owner of file-extent records is a **data-stream ID**, which can differ from the inode ID. If an inode lacks a data stream, its private ID can equal its own inode number. Read the optional data-stream extended field for **actual logical file size** and allocation metadata; do not mistakenly use inode `uncompressed_size` for every ordinary file.

Inode `mode` determines object class (regular file, dir, symlink, device, FIFO, socket where applicable). `bsd_flags` may include compressed/immutable/hidden bits; do not discard it without telling callers. The times are stored with nanosecond precision. A Unix extraction can retain UID/GID/mode/times best-effort while preserving unsupported APFS metadata in sidecar mode.

### 6.4 Extended fields (`xfields`), not the same as xattrs

Several FS records attach optional, packed, typed fields **inside the record's value**. This layout is distinct from `J_XATTR` records:

```text
xfields blob:
  2 bytes  extension_count
  2 bytes  total_field_data_used
  4 bytes × extension_count: { type:u8, flags:u8, byte_length:u16 }
  field values sequentially, with 8-byte alignment between entries
```

All extension descriptor ranges and padding must fit the containing FS record. Unknown extension kinds with a declared length can generally be ignored during reading. For writing, only generate known extensions and zero padding.

| Common inode extension type | Number | Contents / use |
|---|---:|---|
| Snapshot XID | 1 | snapshot/version metadata |
| Delta tree OID | 2 | specialized delta tracking |
| Document ID | 3 | stable document metadata |
| Name | 4 | inode's primary name |
| Previous file size | 5 | historic size |
| Finder info | 7 | 32-byte macOS Finder metadata |
| Data stream | 8 | file size, allocation, crypto and I/O counters |
| Directory stats tree key | 10 | stats record reference |
| Filesystem UUID | 11 | inode UUID |
| Sparse byte count | 13 | sparse logical statistics |
| Device ID (`rdev`) | 14 | device special file payload |
| Original sync root | 16 | specialized clone/sync metadata |

Extended-field flag bits commonly include `DATA_DEPENDENT=0x01`, `DO_NOT_COPY=0x02`, `CHILDREN_INHERIT=0x08`, `USER_FIELD=0x10`, `SYSTEM_FIELD=0x20`; their significance can vary by record/OS behavior. Directory records use e.g. `DREC_EXT_TYPE_SIBLING_ID=1` to link a dirent to a hardlink sibling ID. Parse descriptor types based on **containing record kind**, not a global numeric enumeration.

### 6.5 Data stream extension

The `DSTREAM` extension value has five u64 fields (40 bytes):

| Relative offset | Field | Interpretation |
|---:|---|---|
| 0 | `size` | logical file byte count |
| 8 | `alloced_size` | accounted bytes allocated |
| 16 | `default_crypto_id` | crypto identifier |
| 24 | `total_bytes_written` | cumulative written bytes |
| 32 | `total_bytes_read` | cumulative read bytes |

For extraction, `size` controls the returned byte length. `alloced_size` is useful metadata but is not a byte offset or a substitute for checking extents. A directory commonly has no ordinary file-content data stream. For a compressed inode, transparent logical data length may come from compression metadata and must not be conflated with compressed storage size.

### 6.6 Directory records: plain and hashed formats

A directory entry is a separate FS-tree record owned by its **parent directory inode**. It links a filename to the child inode, and holds the insertion timestamp and entry-type flags.

**Plain-key variant:** 8-byte packed FS-tree key, 16-bit name byte length, then NUL-terminated UTF-8 name.

**Hashed-key variant:** 8-byte packed FS-tree key, 32-bit packed `lengthAndHash`, then NUL-terminated UTF-8 name. The **bottom 10 bits** hold the name's *encoded byte length including terminal NUL*; the **upper 22 bits** hold a normalized name hash. The name bytes themselves are still necessary for collision resolution and case-preserving output.

**Hashed-name calculation (independently described):** starting from UTF-8, apply canonical decomposition (NFD), convert to NUL-terminated UTF-32, compute CRC-32C with the completion/complement convention defined by APFS, retain the low 22 bits, and combine with byte length. When the volume is case-insensitive, its lookup comparison includes the appropriate Unicode case-fold treatment. The precise normalization/case-fold tables and CRC's initialization/finalization must be confirmed with macOS-produced fixtures; OS versions have differed in Unicode details. **Never compare only the 22-bit hash.**

Directory-record **value** has a fixed 18-byte prefix:

| Offset | Width | Interpretation |
|---:|---:|---|
| 0 | 8 | child inode ID (`file_id`) |
| 8 | 8 | directory entry add-time (nanoseconds) |
| 16 | 2 | flags; low bits describe file kind |
| 18 | variable | optional extended fields (e.g. sibling ID) |

The **primary source's hashed-key instructions include terminal NUL in the conversion**. Some third-party descriptions differ in low-level hash conventions, so use cross-checked test vectors, not reimplemented assumptions.

### 6.7 Unicode and extraction semantics

APFS generally stores UTF-8 filenames and supports Unicode normalization-aware matching; volume compatibility bits control case and normalization insensitivity. Stored bytes should be preserved for listing/extraction, and normalized comparison should only be used for lookup/indexing. Explicitly test composed/decomposed forms (e.g. `é` as one code point vs `e` plus combining accent), Turkish dotted/dotless `I`, German sharp s, Greek sigma, and supplementary-plane code points. Host filesystem normalization may differ: a Linux ext4 output can represent two names that an APFS case-insensitive volume cannot. Conversely, exporting two APFS volumes into one host directory can produce collisions.

Proposed fsimg policy: `--name-collision=fail|escape|last-wins`, default `fail` for safe extraction; optionally write an original-name sidecar manifest. Reject NUL, `/`, traversal outside target and dangerous symlink-following during extraction, even though a well-formed APFS filename cannot contain path separator by design.

### 6.8 Extended attributes (xattrs)

Xattrs are their own FS-tree records keyed by inode ID and NUL-terminated attribute name. The suffix is 16-bit name-length plus name bytes. A value begins with `flags:u16` and `xdata_len:u16`, followed by either the payload itself or a data-stream descriptor.

| Value flag | Number | Meaning |
|---|---:|---|
| DATA_STREAM | 1 | payload is in separate APFS data stream |
| DATA_EMBEDDED | 2 | payload follows inside value |
| FILESYSTEM_OWNED | 4 | special metadata owned by filesystem |

For a **stream-backed xattr**, expect the referenced `xattr_obj_id` and a data-stream value (8 + 40 bytes) and read its extents using that stream ID. For an embedded attribute, ensure `xdata_len` stays inside record boundaries. Not every xattr is safe to set on a foreign host filesystem; provide raw extraction or sidecar preservation.

Important names include:

* `com.apple.fs.symlink` — symbolic-link target bytes.
* `com.apple.fs.firmlink` — metadata participating in System/Data unioning.
* `com.apple.decmpfs` — compression metadata/header or compressed payload.
* `com.apple.ResourceFork` — alternate resource fork, also used for some compressed files.
* `com.apple.FinderInfo`, `com.apple.quarantine`, `com.apple.metadata:*` — conventional macOS metadata; preserve where possible.

Treat attribute names as case-sensitive byte strings unless a specific property says otherwise; filename normalization logic must not be blindly applied to xattr names.

### 6.9 Directory statistics

Directory-stats records carry aggregate counts and total byte statistics. They are not substitutes for the directory-record B-tree listing. fsimg readers can expose them as informational metadata; writers should recalculate and validate counts/flags only for the APFS profile being emitted. Test deletions and rename-out-of-directory because these operations influence multiple aggregates.

### 6.10 Namespace-reading algorithm

```text
list_directory(volume, directory_inode):
    validate directory inode mode and ownership in FS tree
    seek to lowest FS key (owner=directory_inode, kind=DIR_REC)
    while key remains in directory-record range for that parent:
        parse plain or hashed name key according to tree/volume flags
        parse drec value, ensure child ID and type valid
        yield {name, child_ino, type, date_added, sibling_id?}

lookup_path(volume, path):
    ino = volume.root_inode
    for component in sanitized path components:
        find matching child drec, including normalized comparisons
        ino = child inode
        handle symlink expansion under depth budget, unless nofollow
    return inode with associated xfields
```

**Reference:** Apple *File-System Objects* pp. 71–101; *Extended Fields* pp. 108–114; *Siblings* pp. 115–116.

## 7. File data, sparse streams, clones and hard links

### 7.1 Normal file extents

File content does not live inline in the inode as a generic stream. Most regular files use filesystem-tree `FILE_EXTENT` records whose owner ID is the inode's **private data-stream ID**. A key is 8-byte packed word followed by an 8-byte **logical offset in bytes**.

An extent value comprises three u64s (24 bytes):

| Value offset | Meaning | Interpretation |
|---:|---|---|
| 0 | `lengthAndFlags` | lower **56 bits**: extent length in bytes; upper 8 bits: flags |
| 8 | `physicalBlock` | first physical APFS allocation block (not byte offset) |
| 16 | `cryptoId` | extent's encryption key/crypto identifier |

Read a file by sorting its extents in increasing logical address, verifying nonoverlap/ordering and filling any uncovered logical byte ranges with zeros (sparse holes). Physical data starts at `store_base + physicalBlock * containerBlockSize`. Truncate the final returned stream to the `DSTREAM.size`, even if the last allocated extent is block-aligned beyond logical EOF.

A file can have many fragmented extents, including physical blocks far from its parent inode or metadata. A clone can reference some or all of the same physical extents as another inode. **Never assume the extents for file F are physical neighbors just because they are logical neighbors.**

### 7.2 Sparse-file reader

```text
read_stream(private_id, logical_size, request_start, request_len):
    verify request interval lies within logical_size
    zero-fill output buffer
    scan FILE_EXTENT keys for this private_id intersecting the interval
    for each extent:
       check logical offset+length arithmetic and physical store bounds
       if encrypted: request key or fail (never return ciphertext as plaintext)
       copy only intersection of logical extent with requested range
    return logical bytes, including zero-filled holes
```

For enormous sparse files, do **not** allocate the logical file size in RAM. Stream the contents in bounded chunks or create a sparse output file with seek/write/truncate. Writer must track logical length independently of allocated physical data and regenerate/update sparse-statistics xfields where required.

### 7.3 Physical extent reference records

The extent-reference tree exists to account for physical ownership and sharing. Each record's key encodes the starting physical block number (in the owning object component). A typical value describes `(extentLengthAndKind, owningStreamOrObject, refcount)`; the length is measured **in blocks**, not in bytes. The lower 60 bits carry block length and the upper four carry extent kind. Refcount is a signed 32-bit quantity in the documented layout.

The reference tree is especially important for a **writer**, because clones and snapshots can retain blocks no longer reachable from the latest ordinary inode records. W0 should avoid sharing file-data blocks at all; W2 can introduce sharing only when refcount accounting, delayed reclamation, transactional OMAP updates and clone test cases are proven.

### 7.4 Data stream ID records

A `DSTREAM_ID` record associates a data-stream ID with its reference count (a u32 value). It allows data streams to have semantics separate from inode identity. A writer that creates, truncates, duplicates, unlinks or updates stream-backed xattrs must adjust stream record lifetimes; deleting the inode's last directory entry does not by itself prove it can free all underlying blocks, especially with snapshots.

### 7.5 Hard links and siblings

Multiple directory entries can refer to the **same inode**, not several copied inodes. The inode reports hardlink count via its `nlink` field. The primary inode `parent_id` and name extension may describe only one of the links. APFS records additional links using sibling-link objects and optionally sibling-map records:

* `SIBLING_LINK` key includes owner inode + sibling ID; value identifies additional parent inode and UTF-8 name.
* `SIBLING_MAP` connects sibling ID to inode ID when the volume enables hardlink maps.
* A directory record can include a sibling-ID extended field.

For extraction to a POSIX filesystem, gather `(volume UUID, inode ID)` identity, extract the file once and use host `link()` for subsequent references when possible. For cross-device extraction or filesystems without hard links, a deliberate copy fallback is allowed but should record loss of link equivalence. Never assume a directory hard link is supported; APFS mainly uses ordinary file hard links and specialized namespace machinery.

### 7.6 Clones and copy-on-write data sharing

An APFS clone is semantically an independently named file whose extents can share physical data until either branch changes. Unlike a hard link, clone modifications must not change the other inode's content. A reader simply follows each file's own extent map; shared paddr runs naturally produce equal bytes. It need not reconstruct which `clonefile()` operation created them. A writer must:

* create distinct inodes / data-stream identities;
* preserve or adjust physical-extent refcounts and shared ownership;
* COW allocated blocks before any modification of one branch;
* account for snapshots retaining older versions of the same data;
* avoid treating every repeated paddr as corruption, while detecting illegal overlapping extents with inconsistent contents/ownership.

W0 writer should materialize distinct payload blocks rather than emit clone sharing; the input-output meaning remains correct even without space-saving clone semantics (provided fsimg does not promise clone preservation).

### 7.7 Symlinks and special nodes

The symlink inode has symlink file kind in POSIX mode and stores the target in `com.apple.fs.symlink`, subject to format conventions/OS version. For extraction, retrieve target as bytes and create a host symlink without following it. Device special nodes use an `RDEV` extended field. Sockets, FIFOs, ACLs and BSD file flags may be impossible or unsafe to reconstruct on an unprivileged host: preserve as metadata in an optional manifest and emit warnings instead of dropping them silently.

**Reference:** Apple *Data Streams* pp. 102–107; *Siblings* pp. 115–116; *File-System Object Constants* pp. 84–101.

## 8. Snapshots and historic views

Snapshots are persistent checkpoints of a **volume namespace and its metadata/data reachability**, not just named directories. The current volume superblock points at a snapshot-metadata tree containing snapshot records and name mappings. Object-map lookups must use the snapshot's historic XID.

### 8.1 Snapshot key/value concepts

* The **snapshot metadata** record is keyed by a snapshot transaction identifier; its value references the snapshot's volume superblock, the relevant historic extent-reference tree, creation/change metadata, flags and snapshot name.
* The **snapshot name** record maps a human-readable name to an XID. The reserved snapshot-name key domain differs from ordinary directory entries.
* The volume OMAP also maintains transaction-versioned versions of virtual objects. Snapshot-specific root and metadata objects can reside at earlier physical addresses.

Do not read current FS-tree root then merely filter leaf records by older timestamp. That would not reconstruct the historic state: current tree nodes might have been replaced wholesale by COW.

### 8.2 Snapshot-reading sequence

```text
open_snapshot(volume, name_or_xid):
    lookup snapshot name/XID in snapshot metadata tree
    load record, validate snapshot references and flags
    select historical volume superblock for target XID
    select snapshot-specific extent references and FS root
    use resolve_virtual(..., target_xid) for all subsequent lookups
    never mutate current volume metadata
    return read-only volume view at XID
```

Snapshot views are logically read-only. If a snapshot is 'dataless' or references data not available in this physical image, report an explicit missing-data state; do not silently synthesize missing content. APFS time-machine backups and sealed boot snapshots may exercise considerably different paths.

### 8.3 Garbage-collection implication

A block whose current OMAP version has been replaced may remain live because an earlier snapshot references it. This is why a writer cannot free prior versions simply by walking the current namespace and subtracting used blocks. Correct reclamation needs version visibility, extent-reference counts and delayed free-space queues; the reaper handles some large metadata cleanup. For W0, create a fresh image without snapshots; for W1, reject snapshot-preserving in-place edit requests.

**Reference:** Apple *Object Maps* pp. 44–50; *Snapshot Metadata* pp. 117–121; *Spaceman* pp. 157–164.

## 9. Transparent compression and resource forks

Compression is an overlay on top of APFS metadata and xattr/data-stream storage rather than a wholly different APFS inode type. Compression details interact with Apple FS compression conventions, which go beyond the core 2020 APFS document. Use Apple-created compressed fixtures to test the exact code paths; do not claim that the APFS superblock alone describes all compression formats.

### 9.1 Identification and decoding

A transparently compressed inode commonly has the BSD `UF_COMPRESSED` flag and a `com.apple.decmpfs` xattr. This attribute begins with a decmpfs header (magic bytes commonly displayed as `cmpf`, compression type and logical/uncompressed length). The remainder may contain compressed bytes inline or refer to `com.apple.ResourceFork` data where payload is chunked.

Commonly observed decoder families include:

| Decimal type | Meaning in conventional decmpfs classification |
|---:|---|
| 1 | uncompressed/inline |
| 3 | zlib inline |
| 4 | zlib via resource fork |
| 7 | LZVN inline |
| 8 | LZVN via resource fork |
| 9 / 10 | uncompressed payload variants |
| 11 / 12 | LZFSE inline / resource fork |
| 13 / 14 | LZBITMAP inline / resource fork |

**Compatibility warning:** not every codec/classification is supported by every macOS version. Resource-fork formats include offsets, chunk tables, compression headers and sometimes uncompressed-chunk passthrough markers. Implement each codec/framing mode against independently produced golden fixtures. Do **not** pass the whole resource-fork stream to a block decompressor without parsing the framing.

### 9.2 Reader behavior

1. Check inode file kind and compression flag; locate the decmpfs xattr.
2. Decode and bound uncompressed logical length and selected codec.
3. If inline, decompress with a streaming or bounded decode API.
4. If resource fork, obtain the underlying xattr stream, read its chunk directory, decode chunks in logical order.
5. Verify output length; enforce maximum expansion ratio and wall-clock/CPU budget.
6. Present logical decompressed contents by default; expose raw compressed streams in an inspection mode.

A BSD-3-Clause potential building block is [Apple's `lzfse` library](https://github.com/lzfse/lzfse), whose [license text](https://github.com/lzfse/lzfse/blob/master/LICENSE) uses a three-clause BSD form. Confirm its available decoder entry points distinguish LZFSE and LZVN; other codec dependencies should be reviewed separately for your code-use policy. Native macOS `libcompression`, `ditto` and Finder are excellent **black-box test oracles** independent of whether their implementations are BSD licensed.

### 9.3 Writer behavior

For W0, write ordinary uncompressed file extents, and **clear UF_COMPRESSED**; this produces correct file bytes even if input was compressed. For an explicit `--preserve-apfs-compression` option, fail on unsupported input codec rather than outputting a misleading inode. Compressed writer support is a separately tested W2 feature requiring byte-for-byte readback on macOS and format-specific resource fork generation.

## 10. Sealed System Volumes (SSV)

Beginning with Big Sur, a System volume may be cryptographically sealed. A valid sealed APFS System volume is characterized by a **System role**, the `SEALED_VOLUME` incompatibility flag, a nonzero integrity-metadata object, and a nonzero specialized file-extent-tree reference. Its filesystem B-tree uses hashing and no-header flags. These are different constraints from an ordinary writable Data volume in the same container.

### 10.1 Integrity metadata

The integrity metadata object includes: object header, metadata version, seal flags, hash algorithm identifier, byte offset of the expected root digest, optional broken-seal XID, and version-dependent reserved fields. Supported digest types documented by Apple include **SHA-256, SHA-512/256, SHA-384 and SHA-512**. The root metadata hash is meaningful only if verified against a **trusted root value**; a matching digest stored in an attacker-modifiable image alone cannot establish origin authenticity.

In a hashed B-tree, internal child references carry a child OID and a fixed-length digest slot up to 64 bytes. The checksum behavior and object-header rules differ in no-header nodes. A secure reader should verify each child node's content digest and chain the verification to the expected root, and should separately verify file-data hashes where the format requires them.

### 10.2 Specialized sealed file-extent tree

Sealed volumes can store file data mappings in a distinct `fext_tree` rather than ordinary filesystem-tree `FILE_EXTENT` records. The relevant key includes a private data-stream ID and logical byte address; the value encodes extent length/flags and physical block location. Some inode data hashes reside in `FILE_INFO` records (record kind 13). **Merely walking the ordinary FS tree and ignoring `fext_tree` will produce an incorrect or incomplete extractor** for sealed System images.

### 10.3 fsimg reader states

Distinguish these states in the API:

| State | Meaning |
|---|---|
| `readable_unverified` | namespace and file content could be read, but seal and Merkle digests not verified |
| `structurally_verified` | APFS object checksums and B-tree invariants verified, no authenticity claim |
| `integrity_verified` | hashes through relevant trees/data match a chosen root |
| `trusted_seal_verified` | integrity root matches an independently trusted digest/chain-of-trust |
| `unsupported_sealed_variant` | cannot follow required tree or digest algorithm |

W0 does not create System/SSV volumes, bless boot objects, sign seals or claim bootability. A normal user-data APFS image is a much smaller target.

**Reference:** Apple *Sealed Volumes* pp. 150–157.

## 11. Encryption and protected storage

APFS encryption includes container/volume keybag metadata, wrapped keys, per-file or volume keys, protection classes and a data/metadata cipher path. Some macOS volumes use a volume encryption key, but iOS protection classes and Secure Enclave / T2 mediation change how keys are obtained. Apple's public reference describes object-level encryption conventions and keybag data types; implementation also requires tested crypto APIs and a safe key-management architecture.

### 11.1 Reader decision tree

* If the volume is explicitly unencrypted, proceed with ordinary metadata reads. A volume can still contain cryptography-related fields with inactive/default values.
* If software-encrypted and the caller supplies an authorized passphrase/key material, choose the appropriate keybag and derive/decrypt the needed keys; map crypto IDs to streams and blocks correctly.
* If hardware-mediated protected state cannot be recovered from the image, fail with `KEY_REQUIRED` or `HARDWARE_KEY_UNAVAILABLE`, not `CORRUPT_IMAGE`.
* Do not try to read ciphertext as if its bytes were a valid B-tree or compressed stream.

### 11.2 Encryption-specific correctness requirements

A separate encryption implementation needs test vectors for: keybag formats, key-wrap algorithms, KDF parameters, AES-XTS sector tweak calculation and sector alignment, metadata key derivation, software key rotation, per-file encryption IDs, and decryption of content and metadata after clone/snapshot. The format includes fields for encryption generation; a numeric physical block and volume XID are not sufficient to choose keys without considering stored crypto IDs.

`go-filesystems/apfs` (BSD-3-Clause) describes an experimental/full read-write encrypted implementation in its README. Its self-reported encrypted `fsck_apfs` behavior has special caveats and must **not** be taken as proof that every Apple encryption mode is supported. For fsimg's image-generation mission, encryption is a well-defined future feature, not a prerequisite for shipping R0–R2 or W0.

### 11.3 Security and licensing

Avoid embedding private macOS framework behavior, Secure Enclave secrets or proprietary Apple implementation details. Test black-box native OS behavior on **your own** fixtures, public on-disk specification, and BSD-licensed code only. Cryptographic libraries themselves may have OpenSSL/Apache/other licenses; if the restriction literally permits **only BSD-licensed copied code**, make that a dependency-review gate. Calling an OS-provided crypto API without copying its implementation is a separate question to decide within fsimg's license policy.

**Reference:** Apple *Encryption* pp. 135–149.

## 12. Space manager, free-space queues, Fusion and reaper

### 12.1 Allocation hierarchy

APFS allocation state is container-wide, not per ordinary volume. The ephemeral **spaceman** object includes the global block size, blocks per allocation chunk, chunks per chunk-information block, chunk-info blocks per address block, device-level space counters, internal-pool bitmaps and references to free-space queues. In a single-store container, main-device space tracking is sufficient. Fusion adds a second tier and more address routing/metadata complexity.

A useful hierarchy:

```text
container blocks
  -> device (main / optional Fusion tier-2)
     -> allocation chunk (fixed number of blocks)
        -> chunk-info record: start block, block count, free count, bitmap paddr, xid
           -> bitmap: allocation state for individual blocks
     -> chunk-info block (CIB): records for nearby chunks
     -> chunk-info address block (CAB): addresses of CIBs
  -> internal pool for metadata
  -> free queues for deferred reclamation / transaction-safe reuse
```

### 12.2 Chunk-information structures

The spaceman tracks device-level counts and references to CIB/CAB and bitmap storage. A common chunk record contains:

| Field | Size | Role |
|---|---:|---|
| `chunk_xid` | 8 | last modifying transaction |
| `chunk_address` | 8 | first block of chunk |
| `chunk_block_count` | 4 | number of allocation blocks |
| `chunk_free_count` | 4 | number of unallocated blocks |
| `bitmap_address` | 8 | physical location of bitmap or special marker |

A CIB starts with object header plus index/count and then repeated chunk records. A CAB begins with object header plus index/count and a list of physical CIB block addresses. Verify all counts and ranges. The bitmap bits must agree with used metadata and data extents **at the selected checkpoint**; validation tools may implement a cross-tree reachability and accounting pass.

### 12.3 Free-space queue semantics

APFS has multiple free-space queues, including an internal-pool queue and main/tier-2 queues. Queue keys involve transaction IDs and physical addresses; values encode runs of blocks, with special handling for ghost records. These queues help ensure that blocks only become allocatable when relevant older transactions and snapshots no longer need them.

For W0, create a clean single-transaction image where all allocated blocks are accounted for and there are no historical versions to retire. For a general transactional writer, one cannot safely choose arbitrary zero bitmap bits without integrating delayed reclamation and checkpoint visibility. `fsck_apfs` will detect allocation tree inconsistencies even if directory extraction appears correct.

### 12.4 Reaper

The reaper manages cleanup of metadata that cannot be removed instantly, such as large trees and objects associated with deleted volumes or snapshots. It maintains transaction-progress state and separate cleanup task entries. A reader mostly checks mapping validity and can ignore worklist contents unless they affect selectable checkpoints. A writer that modifies existing images must not discard or fake in-progress reaper state; this is a major reason to avoid in-place modification initially.

### 12.5 Fusion handling

Fusion APFS maps one logical container onto main and second-tier devices and includes dedicated physical-address encodings, device counters and allocation-zone logic. A single raw byte buffer cannot interpret all physical addresses without an additional physical-store router. For R0, detect `NX_INCOMPAT_FUSION` and return a clear error naming missing second-store support. Do not silently interpret tier-2 paddr as a normal main-device block.

**Reference:** Apple *Spaceman* pp. 157–164; *Reaper* pp. 164–166; *Fusion Containers* (within the container/space-management chapters).

## 13. New-image APFS writer: W0 target profile

### 13.1 Explicit profile, not general read-write mount

Initial W0 format profile:

```text
image kind:       raw APFS container, optional GPT wrapper as independent layer
store count:      one
block size:       4096
containers:       one
volumes:          one, unencrypted and unsealed, regular Data or role-none
snapshots:        none
clones:           disabled (materialize copies)
hardlinks:        optional after base support
content:          regular files, directories, symlinks, ordinary xattrs
compression:      disabled, raw bytes emitted
COW history:      one constructed checkpoint, later staged final checkpoint(s)
```

Choose a minimum practical size **through native testing**. Allow a configurable volume name, case-sensitivity, normalization choice, deterministic UUIDs/timestamps for tests, and byte-order-safe metadata serialization. The only supported mutation API in W0 can be `fsimg create input_dir output.apfs`: it need not support a general random-access writable mount.

### 13.2 Writer internal architecture

Organize the writer around a **logical namespace IR** and a bottom-up serializer:

```text
filesystem tree of logical entries
  -> names, inodes, stream IDs, xattrs, link groups and metadata counts
  -> allocate data blocks and encode data extents
  -> encode FS tree leaves / internal index nodes
  -> encode extent-ref and stream-ID trees
  -> encode volume OMAP, APSB and required private metadata
  -> encode container OMAP and space-accounting structures
  -> encode checkpoint map(s), spaceman and NXSB checkpoint
  -> validate all references, checksums, bitmaps and counts
  -> commit to image as a complete block-addressed structure
```

This is a better fit for fsimg's **create/extract** goal than a general APFS updater. You know the complete directory tree at build time and can pack and sort metadata once. An in-memory object-ID map can allocate stable IDs before physical positions are known; physical block allocation happens in a separate pass. The serializer has to handle circular-looking references between allocator bitmaps, object maps and checkpoint records by reserving block numbers for metadata first and filling those blocks after finalizing child locations.

### 13.3 Mandatory initialization structures

At a minimum, a native APFS container will require the following coordinated objects:

* Container superblock with correct UUID, capacities, features, correct volume-OID array, checkpoint region, OMAP and spaceman pointers.
* Physical container OMAP object and its B-tree mapping the volume's virtual superblock.
* Volume superblock whose tree IDs and counters match emitted objects, with correct metadata creation stamps and volume name.
* Volume OMAP mapping root FS-tree object IDs and other virtual objects used in the image.
* FS-tree root and enough leaf/internal nodes to describe all files/dirs.
* Root directory inode, special `private-dir` records and any other native-required reserved object records; identify exact minimal set by inspecting **both** Apple-produced images and BSD-licensed writer fixtures.
* File-extent records for each ordinary data stream and physical-extent refs for blocks with defined ownership.
* Snapshot/extent reference tree roots, possibly empty but valid where required by the profile.
* Spaceman, CIB/CAB and allocation bitmaps describing **every reserved and allocated block**.
* Checkpoint maps for all required ephemeral objects; a complete published NXSB state; all applicable per-object checksums.
* Any metadata Apple expects to exist even in a nearly empty fresh volume, such as creation/modified-by history, free queues and reaper setup in accepted forms.

The exact minimal writer layout must be fixture-driven; the 2020 public reference documents formats but does **not** constitute a turn-key `mkfs` build algorithm.

### 13.4 Bottom-up B-tree construction

```text
build_tree(sorted_records, size_limit, codec):
    assign records to leaf pages until capacity reached
    serialize leaf TOC, key area, value area, root/nonroot flags
    record each page's separator key and object identifier
    while more than one page at current level:
        build parent index pages from (separator_key, child_oid)
    designate final page root and append btree_info footer
    write checksums and reserve physical mappings for all nodes
```

The actual splitting rule must leave room for variable-width keys, values, TOC entries and root metadata footer; arbitrary `nkeys=32` would fail on long names and xattrs. For a B-tree with a single root leaf, one node can be sufficient if its contents fit. When assigning virtual OIDs, generate corresponding OMAP records. When assigning physical OIDs, the OID usually corresponds to physical placement.

### 13.5 Original staged commit model

W0 can build the image **entirely off-line** and atomically rename the completed file in the host directory. No APFS-specific crash recovery is required while constructing an *unpublished* output file. Validate block references and `fsync()` the completed temporary file before renaming it if durability matters.

For a future W1 writer that commits changes to an already published image, transactions must obey these conceptual constraints:

1. Select latest complete checkpoint XID `T` and reserve `T+1`.
2. Allocate never-overwritten physical blocks for new payload and replacement metadata.
3. Construct new FS-tree paths and OMAP mappings; retain prior versions until they are no longer observable.
4. Update extent references, free-space queue and spaceman in COW-safe order.
5. Write and persist all data/metadata objects on which the new checkpoint depends.
6. Publish checkpoint descriptors/maps and an NXSB representing a **complete** new state.
7. Persist publication; only afterward consider old blocks eligible for safe reclamation.
8. Re-open the image with an **independent reader** and verify both selected checkpoint and (where still valid) prior checkpoint.

This is an **ordering contract**, not a claim that arbitrary placement of an NXSB at the end magically implements APFS commit. In particular the descriptor ring's replacement policy, checkpoint mapping flags and spaceman changes have to match what Apple's mount code accepts. Use an Apple-generated native image as the primary writer acceptance oracle.

### 13.6 Avoid these easy but fatal writer mistakes

| Mistake | Likely symptom |
|---|---|
| Forgetting special root/private-dir records | native mount fails although `NXSB` parses |
| Writing volume APSB directly but no container OMAP entry | volume enumeration fails |
| Treating virtual tree OIDs as physical addresses | B-tree resolution fails |
| Serializing raw packed FS-tree keys in lexicographic byte order | directory scans/search fail |
| Reusing data block across files without extent ownership accounting | `fsck_apfs` errors / corruption |
| Missing OMAP tombstones or choosing wrong XID | stale records reappear |
| Forgetting free-space bitmap for metadata blocks | fsck rejects allocation state |
| Wrong negative offsets in B-tree values | invalid node records |
| Incorrect root-node footer or inconsistent flags | tree not recognized |
| Miscomputed checksum or checksum over wrong byte count | object rejected |
| Confusing UUID byte order or GPT partition start | image not discovered |
| Using normal file extents for sealed System image | missing data / broken seal |

### 13.7 Reproducible writer mode

Provide `fsimg create --reproducible` with fixed deterministic UUID seed, fixed nanosecond timestamps, stable sorted input traversal, deterministic object-ID allocation, deterministic extent placement and zeroed reserved bytes. This yields identical raw APFS bytes from equal logical inputs, invaluable for fuzzer minimization and cross-platform tests. UUID collision risks are real if the resulting images are mounted concurrently, so keep deterministic UUIDs explicitly opt-in.

## 14. Existing implementations and usable references — with licenses

**License rule for this project:** **Only BSD-licensed implementation code may be copied/adapted into fsimg.** MIT, Apache-2.0, zlib, LGPL, GPL, Apple Public Source License, proprietary macOS binaries, and unspecified-license code should be treated as **external references, behavioral test oracles, or independent study only** under this stricter-than-ordinary permissive policy. An external tool can be run against test fixtures without copying its source code. Review *individual source files and transitive dependencies* before accepting even a BSD-labelled repository: GitHub's repository badge is not a guarantee every vendored dependency shares its license.

### 14.1 Primary public format documentation

1. **Apple, [Apple File System Reference](https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf) (2020-06-22).** Primary facts for physical objects, checkpoints, OMAP, FS record families, encryption, sealed volumes, spaceman and the reaper. Public documentation, **not BSD-licensed source code**; derive facts and write original code. It does not specify every new macOS implementation behavior.
2. Apple, [APFS FAQ / File System Programming Guide](https://developer.apple.com/library/archive/documentation/FileManagement/Conceptual/APFS_Guide/FAQ/FAQ.html). Semantics and user-visible behavior; not a substitute for the on-disk reference.
3. [APFS documentation produced by the libyal project](https://github.com/libyal/libfsapfs/blob/main/documentation/Apple%20File%20System%20%28APFS%29.asciidoc). Independently developed secondary format notes with practical edge cases. **Documentation only; do not lift text or code** and cross-check assertions with Apple's format definition.
4. [macOS `diskutil` man page](https://keith.github.io/xcode-man-pages/diskutil.8.html), [macOS `hdiutil` man page](https://keith.github.io/xcode-man-pages/hdiutil.1.html), [macOS `fsck_apfs` man page](https://man.freebsd.org/cgi/man.cgi?manpath=macOS+13.6.5&query=fsck_apfs&sektion=8), [macOS `newfs_apfs` man page](https://man.freebsd.org/cgi/man.cgi?apropos=0&manpath=macOS+13.6.5&query=newfs_apfs&sektion=8). CLI behavior/version-dependent; native utilities are authoritative practical compatibility oracles, **not source dependencies**.
5. [macOS `fs_snapshot_create(2)` man page](https://github.com/apple/darwin-xnu/blob/main/bsd/man/man2/fs_snapshot_create.2). API description. Snapshot creation/mount/revert may require privileged entitlements even as root; test on a disposable device and make fixture availability optional in CI.

### 14.2 BSD-licensed code potentially eligible to study and reuse

| Project | Language / license | Useful functionality | Eligibility / cautions |
|---|---|---|---|
| [go-filesystems/apfs](https://github.com/go-filesystems/apfs) | Go, **BSD-3-Clause** (verified at [pkg.go.dev](https://pkg.go.dev/github.com/go-filesystems/apfs)) | Pure-Go raw/GPT container reader and writer; OMAP, B-trees, FS tree, native formatting, multi-volume, xattrs, compression, snapshots; claims encrypted image support and Apple mount/fsck parity | **Primary BSD-compatible code reference.** New (v0.3.0 published October 2026), limited independent adoption. Verify source LICENSE, dependencies, data fixtures and every interoperability claim. Direct reuse may demand substantial Go-to-C translation for fsimg. |
| [go-filesystems/apfs — compatibility matrix](https://github.com/go-filesystems/apfs/blob/main/COMPAT.md) | Documentation in above BSD project | Claims/tests describing what native macOS accepts, snapshot/metadata corner cases | Valuable test-plan source, but results are author-reported; rerun on your own macOS release. |
| [go-filesystems/apfs — manual cross-platform checks](https://github.com/go-filesystems/apfs/blob/main/COMPAT_MANUAL.sh) | Repository's test script; check per-file header | Lists Apple/read/write interop procedures | Good inspiration for an independently authored fsimg test harness. Don't transplant without per-file license check. |
| [lzfse/lzfse](https://github.com/lzfse/lzfse) | C, **BSD-3-clause text** ([license](https://github.com/lzfse/lzfse/blob/master/LICENSE)) | LZFSE / LZVN compression primitives used by APFS `decmpfs` | Eligible BSD code if build-system/dependency audit passes. Chunk/resource-fork framing is still fsimg's responsibility. |
| [go-darwin/apfs](https://github.com/go-darwin/apfs) | Go, reported BSD-3-Clause | Wrappers for Darwin/APFS-related host interfaces | **Not an independent APFS on-disk parser**. Relevant only for macOS test harnesses; check per-file licensing. |

**Recommendation:** independently implement the parser using Apple's public layout facts, and use the BSD-3 Go project as a *reviewable cross-check of difficult structural assumptions*. It may offer useful benchmark fixtures and test ideas. Do not import code before source/dependency audit.

### 14.3 Non-BSD implementations: valuable tests, not code to copy

| Project | License reported | Reader / writer / focus | How fsimg should use it |
|---|---|---|---|
| [sgan81/apfs-fuse](https://github.com/sgan81/apfs-fuse) | GPL-2.0 | Linux FUSE **read-only** APFS; snapshots, sealed volumes, software encryption, Fusion, GPT/DMG; partial compression; no firmlinks | Independent `mount`/`cat`/hash oracle, help triage R1–R4, **no copied code** |
| [linux-apfs/linux-apfs-rw](https://github.com/linux-apfs/linux-apfs-rw) | GPL-2.0 | Experimental Linux kernel driver with APFS read/write paths | Separate mount/write interoperability, VM only, never trusted host volumes |
| [linux-apfs/apfsprogs](https://github.com/linux-apfs/apfsprogs) | GPL-2.0 | `mkapfs` (experimental writer), `apfsck` (nonrepairing checker), `apfs-label`, `apfs-snap` | Best open Linux W0 writer/checker oracle; **do not borrow implementation** |
| [libyal/libfsapfs](https://github.com/libyal/libfsapfs) | LGPL-3.0-or-later library; GPL tooling in repository | Forensics/read library, `fsapfsinfo`, `fsapfsmount`; compression/unencrypted and certain encrypted images, xattrs; snapshots, Fusion, T2 unsupported in its README | Independent read/extract and metadata oracle; good parser robustness comparison |
| [blacktop/go-apfs](https://github.com/blacktop/go-apfs) | Apache-2.0 (verify actual repository state) | Go read/inspection tools, file listing/extraction; state varies by revision | Additional read-only disagreement oracle; **no code copying** |
| [deploymenttheory/go-apfs-v2](https://github.com/deploymenttheory/go-apfs-v2) | MIT (verify LICENSE) | Go APFS/HFS+ command, publicly claims read/write, DMG, snapshots, compression and FileVault | Additional W0/W1 cross-platform image generator/reader; treat claims cautiously, no source reuse |
| [avighnac/libapfs](https://github.com/avighnac/libapfs) | MIT | New read-only C++ library and `apfs` CLI (`ls`, `cat`, container info); docs and packaged CLI | Convenient independent native or Linux reader test, not copied code |
| [tienex/apfs](https://github.com/tienex/apfs) | MIT | Older APFS inspector, mounting/dumping | Alternate historical and structural behavior reference |
| [cugu/apfs.ksy](https://github.com/cugu/apfs.ksy) | MIT | Kaitai Struct schema for selected APFS structures | Fast triage/byte-level visualization; **do not copy schema/code** |
| [cugu/afro](https://github.com/cugu/afro) | GPL-3.0 | APFS recovery/forensics tool | Corruption and recovery-fixture oracle |
| [The Sleuth Kit](https://github.com/sleuthkit/sleuthkit) | mixed/project-specific | forensic `fsstat`, `fls`, `icat` etc., with build-dependent APFS coverage | Optional metadata extraction/disagreement oracle after verifying `-f apfs` support in installed build |
| [Apple open-source distributions](https://github.com/apple-oss-distributions) | Mixed, often **APSL 2.0** | `diskdev_cmds`, HFS and generic storage interfaces; **not** public complete APFS kernel source | macOS CLI/architecture context only; do not transplant APSL code |

The presence of a tool in this list is **not a claim it is actively maintained, free of vulnerabilities, feature-complete, or suitable to mount an irreplaceable drive**. Run all experimental readers/checkers against copied images. In particular, `apfsck` warns about handling maliciously crafted input and does not repair corruption.

### 14.4 Useful manuals and repository subresources

* [apfsprogs `mkapfs(8)`](https://manpages.debian.org/testing/apfsprogs/mkapfs.8.en.html): verified syntax `mkapfs [-sv] [-L label] [-U UUID] [-u UUID] [-F tier2] [-B tier2-blocks] device [blocks]`; `-s` changes case sensitivity, `-z` controls normalization on builds that include it; `-F` configures tier-2 Fusion.
* [apfsprogs `apfsck(8)`](https://manpages.debian.org/trixie/apfsprogs/apfsck.8.en.html): verified syntax `apfsck [-cuvw] [-F tier2] device`; `-u` reports unsupported features, `-c` reports crash indicators, `-w` extra inconsistencies. **Checks only, no repair.**
* [apfs-fuse README](https://github.com/sgan81/apfs-fuse#readme): `apfs-fuse image mountdir`, `-v N` selects volume, `-p N` partition, `-s N` APFS offset, optional snapshot selector `-o snap=XID`.
* [libfsapfs `fsapfsinfo(1)`](https://github.com/libyal/libfsapfs/blob/main/manuals/fsapfsinfo.1): `fsapfsinfo -o OFFSET image.raw` for APFS within a larger disk image; inspect `-h` on your build.
* [go-apfs-v2 commands](https://github.com/deploymenttheory/go-apfs-v2#readme) and [implementation status](https://github.com/deploymenttheory/go-apfs-v2/blob/main/TOOLS_STATUS.md): claims are useful hypotheses for selecting differential test families.
* [libapfs CLI usage](https://github.com/avighnac/libapfs/blob/main/README.md): `apfs ls DEVICE --path /`, `apfs cat DEVICE --path /file` examples.

## 15. Test tools: who reads, writes and validates APFS?

| Platform | Tool | Create/write | Read/list/extract | Structural verification | Reliability judgment |
|---|---|---|---|---|---|
| macOS | `hdiutil create` / `attach` | **yes**: makes native DMGs; attached writable APFS volume behaves normally | **yes**: mount and ordinary POSIX tools | n/a | primary macOS fixture producer |
| macOS | `diskutil apfs` | **yes**: container/volume management, native encryption options | **yes**: enumerate roles, volumes, snapshots | partial | native management oracle |
| macOS | `newfs_apfs` | **yes**: raw APFS formatting of disposable device node | no user file extraction | creates native metadata baseline | best direct `mkfs` oracle; destructive |
| macOS | `fsck_apfs -n` | **no** (nonrepair mode) | no | **yes**: Apple's own APFS checker | primary W0 acceptance gate; use unmounted clone |
| macOS | `xattr`, `ls -lO`, `stat`, `cp -c`, `ditto`, `tmutil` | writes ordinary files, attrs, clones, snapshots where permitted | reads outputs | content/metadata oracle | cross-check native semantics |
| Linux | `mkapfs` from apfsprogs | **yes**, experimental | no | no | secondary independent mkfs |
| Linux | `apfsck` from apfsprogs | no | no | **yes**, no repair | useful second structural opinion |
| Linux | `apfs-fuse` | no | **yes**, FUSE | limited | independent read oracle |
| Linux | `fsapfsinfo`, `fsapfsmount` | no | **yes**, FUSE/info | limited | independent parser oracle |
| Linux | `linux-apfs-rw` driver | **yes**, experimental | **yes** | filesystem-level consistency via driver | VM only, copied images |
| macOS/Linux/Windows | `go-filesystems/apfs` Go library/tests | **yes**, claims native-compatible formatting | **yes** | interoperability tests, author-reported | BSD-compatible cross-check |
| macOS/Linux/Windows | `go-apfs-v2` command | **yes**, claimed | **yes** | authored tests | MIT, external oracle only |
| Linux/macOS | `avighnac/libapfs` CLI | no | **yes** | read disagreement | MIT, secondary |
| forensic platforms | The Sleuth Kit | no | build-dependent APFS inspection | limited | conditional upon installed build |

**Most important compatibility checks are (1) `fsck_apfs -n`, (2) native macOS `hdiutil attach`/file I/O, and (3) a separate reader/checker (`apfsck`, `apfs-fuse`, or libfsapfs).** A writer passing only fsimg's own reader is not evidence of a valid APFS image.

### 15.1 Create a native macOS fixture (safe, disposable file)

The following shell commands use a **new file image**, not a physical drive. Review mount paths and `diskN` identifiers before use. macOS volume creation and mount naming may change across releases.

```sh
mkdir -p apfs-fixture && cd apfs-fixture
# Create a writable UDIF disk image containing a normal APFS filesystem.
hdiutil create -size 512m -fs APFS -volname APFSTest -format UDRW apfstest.dmg
# Attach and show the dynamically assigned disk identifier/volume path.
hdiutil attach apfstest.dmg
diskutil list
diskutil apfs list
# Adapt the mount path if macOS suffixes a colliding volume name.
mkdir -p /Volumes/APFSTest/dir/sub
printf 'file-one\n' > /Volumes/APFSTest/dir/sub/a.txt
cp /Volumes/APFSTest/dir/sub/a.txt /Volumes/APFSTest/dir/sub/copy.txt
ln /Volumes/APFSTest/dir/sub/a.txt /Volumes/APFSTest/dir/sub/hardlink.txt
ln -s a.txt /Volumes/APFSTest/dir/sub/symlink.txt
xattr -w com.example.fsimg hello /Volumes/APFSTest/dir/sub/a.txt
# Optional: native APFS clone, on macOS versions supporting cp -c.
cp -c /Volumes/APFSTest/dir/sub/a.txt /Volumes/APFSTest/dir/sub/clone.txt
ls -laiO@ /Volumes/APFSTest/dir/sub
# Detach using the actual attached disk identifier (example disk5).
hdiutil detach /dev/diskN
```

The `.dmg` has an **outer UDIF layer** and may contain GPT. For R0, produce a raw physical-store fixture by exposing its attached `/dev/rdiskNsX` store or by materializing a raw virtual-disk representation from the DMG. Preserve a manifest recording whether the golden bytes are `UDIF`, `GPT` or `RAW_APFS`. When using `hdiutil convert -format UDTO` or `-format UDRW`, do not assume the output is a headerless APFS partition: inspect `diskutil list` and physical start offsets. The raw APFS container starts at the APFS physical-store partition origin, not the start of the DMG file.

### 15.2 Native `fsck_apfs` verification

Check **read-only and off-line** whenever possible. Command-line specifics differ slightly by macOS release, so check `man fsck_apfs` on the test host.

```sh
# Attach the disposable DMG without mounting filesystem contents.
hdiutil attach -nomount apfstest.dmg
diskutil list
# Select the correct APFS synthesized volume or physical store from output,
# with a raw-device path that fsck_apfs expects. NEVER guess disk numbers.
sudo fsck_apfs -n /dev/rdiskNsX
# Expected for W0 unencrypted: clean exit / no structural errors.
hdiutil detach /dev/diskN
```

Do not claim generic `fsck_apfs -n` can verify keybags for every encrypted image; encrypted volumes may need an unlock flow and the result can depend on key availability. `-n` is deliberately **no modify/no repair**; never run corrective/reformatting commands on original customer media.

### 15.3 Test APFS multi-volume behavior

```sh
hdiutil attach apfstest.dmg
diskutil apfs list
# Replace diskN with the synthesized APFS container reference, not partition ID.
diskutil apfs addVolume diskN APFS SecondVolume
# Identify its mount path from diskutil apfs list, then create test data there.
# Verify fsimg enumerates two distinct volume UUIDs in one shared container.
diskutil apfs list -plist > apfs-layout.plist
hdiutil detach /dev/diskN
```

Run another fixture for a case-sensitive APFS personality using native `diskutil` or a fresh `hdiutil` image; do not mix case-sensitive test conditions into a case-insensitive fixture unless the actual volume personality matches.

### 15.4 Snapshot fixtures: read/list versus create

`diskutil apfs listSnapshots /Volumes/APFSTest` is useful for inspection on supported macOS releases. `tmutil` may create local backup snapshots under normal Time Machine conditions. The direct `fs_snapshot_create(2)` system call is documented but can require a privileged entitlement beyond UID 0, and `fs_snapshot_mount` or revert is likewise restricted. Thus:

* Make **snapshot-list/read tests** mandatory only when an existing snapshot fixture is supplied.
* Make **snapshot-create tests** conditional on a macOS host where authorized creation succeeds.
* Preserve a gold image with a snapshot created on an appropriate machine instead of requiring CI to generate it on every run.
* For tests, create data version A, take snapshot, mutate live data to B, and verify that fsimg's historic view returns A while the live view returns B.

### 15.5 Linux external validators

Run tools against **copied** images and follow the correct raw/GPT offset model:

```sh
# Debian/Ubuntu packaged tools where available. Names depend on distribution.
fsapfsinfo -h
fsapfsinfo -o 0 apfs-container.raw

# GPL APFS FUSE oracle; select volume number for multi-volume images.
mkdir -p /tmp/apfs-mount
apfs-fuse -v 0 apfs-container.raw /tmp/apfs-mount
find /tmp/apfs-mount -type f -print
# unmount with fusermount3 -u /tmp/apfs-mount (or system's FUSE utility)

# GPL open-source structural checker, no repair.
apfsck -u apfs-container.raw

# GPL experimental generator; THIS OVERWRITES ITS TARGET.
truncate -s 512M experimental-empty.raw
mkapfs -L APFSTest experimental-empty.raw
apfsck experimental-empty.raw
```

If `fsapfsinfo` is given a whole GPT disk image, supply the **partition offset in bytes**. `apfs-fuse` can discover GPT and has explicit partition and offset options; see its README. `apfsck` may require a raw APFS container rather than a GPT-wrapped disk. The experimental `mkapfs` result should be separately checked by native macOS tools before using it as proof of canonical layout.

### 15.6 Cross-writer interoperability matrix

| Generator → consumer | fsimg reader | Native macOS mount/read | Apple `fsck_apfs -n` | Linux `apfsck` | Third-party reader |
|---|---|---|---|---|---|
| Apple `hdiutil` → | **must pass** | baseline | clean baseline | inspect | independent read |
| Apple `newfs_apfs` + copy files → | **must pass** | baseline | clean baseline | inspect | independent read |
| Linux `mkapfs` → | **must pass for advertised subset** | investigate acceptance | investigate | own checker | independent read |
| BSD Go `FormatContainer` → | cross-check | independently retest mount | independently retest | inspect | independent read |
| fsimg `W0` → | **must pass** | **must mount and read all files** | **must be clean** | must be clean or explicitly explain differing checker | independent reader agrees |
| fsimg `W1` add/write → | must read | Mac writes a new file and reads old files | clean after native and fsimg changes | inspect | independent reader agrees |

For every writer output, also verify **macOS can add a new file after mounting and commit it**, not merely that a directory listing works. A nominally mountable image with incorrect spaceman state can fail on its first real write.

## 16. Specification-driven validation and fuzzing plan

APFS's on-disk description lends itself to a **declarative constraint corpus** that drives a parser fuzzer, rather than just changing random bytes. The generator does not need a theorem prover or full model checker; most structural constraints are local arithmetic and cross-reference checks.

### 16.1 Typed invariants to express as data

| Record | Legal constraints | Example invalid inputs |
|---|---|---|
| Object header | block size, type/flags permitted, checksum correct, XID in supported range | wrong checksum, invalid storage flags, incorrect OID, XID zero where prohibited |
| NXSB | magic, block size power-of-two/in range, count fits physical storage, descriptor/data rings internally consistent | ring count zero/overflow, 100 volume OIDs with maxFS=1, base outside file |
| Checkpoint map | count fits block, map complete, mapped object type and address plausible | missing `LAST`, duplicate same-OID mappings, address alias, referenced block missing |
| OMAP | ordered `(oid,xid)` records, object size fits, predecessor lookup rules | duplicate same key, deleted version ignored, wrong physical address |
| B-tree | TOC spans fit, separator ordering, levels decrease, no cycles, root footer valid | TOC overflow, `v.off=0xffff` without ghost flag, huge height, key out of order |
| FS inode | at least 92 fixed bytes, valid mode and xfields, data stream size sane | truncated xfields, invalid extensions, huge DSTREAM count |
| Dir record | NUL termination, length correct, hash correct for volume rules, child inode exists | path separator, bad name hash, dangling inode, invalid UTF-8 |
| File extent | nonoverlapping logical ranges; physical allocation in store; logical size respected | wraparound logical end, overrun physical store, extent pointing to metadata |
| Xattr | length fits or stream descriptor points to valid extents | stream pointer cycles, attribute expansion bombs |
| Snapshot | valid XID and OMAP root, historic roots reachable | snapshot points to newer-only object, historic tombstone ignored |
| Spaceman | free/used bit accounting and allocated metadata consistent | used file block marked free, counted blocks exceed device |
| Sealed volume | required references exist, supported digest type, verified child hash | wrong digest, missing file-extent tree, spoofed integrity status |

### 16.2 Fuzz targets

Implement separate `libFuzzer`/AFL++ targets with one input object each, plus end-to-end targets:

1. `apfs_probe`: random short bytes, GPT, UDRW/UDIF adapters and NXSB block-size manipulation.
2. `apfs_checkpoint`: descriptor/data ring arithmetic, duplicate and partial XID sets, huge mapping counts.
3. `apfs_obj_decode`: object header/checksum/type dispatch, unknown flags.
4. `apfs_omap`: synthetic sorted/unsorted OMAP B-tree, ancestor/newer/tombstone XIDs.
5. `apfs_btree_node`: every TOC mode, root/nonroot, footer boundary, ghost/free list and hashed node.
6. `apfs_fs_record`: every FS key subtype and value, especially xfields and UTF-8 names.
7. `apfs_file_stream`: sparse gaps, overlapping extents, near-2^64 offsets, multi-read chunking.
8. `apfs_decmpfs`: truncated headers, chunk offsets, codec output-size bombs, wrong stream sizes.
9. `apfs_open_image`: mutated whole images, corrupted checkpoint 0 vs valid later checkpoint.
10. `apfs_roundtrip`: generate a valid random namespace, serialize W0, read back and compare semantic manifest.

### 16.3 Three classes of mutation

* **Byte mutation:** mutate valid fixture images with arbitrary bit flips and truncation; expected outcome is either correct data or clean failure, never OOB/UB/crash/hang.
* **Checksum-repaired semantic mutation:** modify one field, then update affected object checksum(s) using fsimg's own serializer so the reader reaches deeper invariant checks. Useful for testing numeric overflows and B-tree invalid spans.
* **Valid-by-construction structure mutation:** generate B-tree records and superblocks from a declarative schema with legal offsets/counts. Vary number of records, tree height, key sizes, names, snapshots and extent fragmentation. For W0 outputs, check native interoperability.

A raw fuzzer will spend most inputs failing NXSB checksum; checksum repair and parser-unit fuzzing are therefore essential to achieving branch coverage deeper than probe logic.

### 16.4 Semantic round-trip properties

Use a canonical manifest containing volume UUID/role/name, normalized path identity, original filename bytes, file-kind/mode/UID/GID/nanosecond times, hardlink groups, size, content SHA-256, symlink target, xattr name/payload hashes and snapshot XIDs. Important invariants:

```text
read(fsimg_write(T)) == canonicalize(T)          for W0-supported tree T
read_file(path, 0, 0) == empty_bytes
concat(read_file(path, split chunks)) == full read
read_file(cloneA) == read_file(cloneB)          immediately after clone
hardlink(path1).inode == hardlink(path2).inode
snapshot(X).file == old_data                    after live file changed
allocated_extents are wholly inside container
all B-tree keys are sorted by their tree-specific comparator
all reached metadata object checksums are valid
fsck_apfs -n(fsimg_write(T)) == clean             for W0 unencrypted
```

Random-size filename fixtures should cover 1, 7, 8, 9, 255, 256, and near maximum accepted UTF-8 bytes; content fixtures should cover 0, 1, 4095, 4096, 4097, 65535, 65536, 65537 bytes plus >4 GiB logical sparse files without allocating a >4 GiB output in memory. Force >1 B-tree node and >1 B-tree level using thousands of entries.

### 16.5 Power-cut and checkpoint recovery tests

Start from a valid committed image. Have a test-only staged writer generate a second transaction with a write-trace of physical blocks. Produce truncated/written-prefix copies after every important stage, including new data, new tree leaves, new OMAP nodes, checkpoint maps and final NXSB updates. The reader's selected XID must be either an *older complete* state or the *new complete* state, never a hybrid. For a **new-image** W0 writer the simpler transaction requirement is atomic temp-file publication.

### 16.6 Oracle disagreements

When Apple's native mount succeeds but `apfsck` rejects an image, or vice versa, classify it by implementation, OS, version, unsupported feature and error details. Do not weaken fsimg's validity checks solely to match one third-party reader. An oracle can have bugs too. Record a minimized public-input fixture and a machine-readable explanation of which invariant is disputed.

## 17. Suggested fsimg API and code layout

Keep the format implementation independent of platform and outer container. APFS is not stream-sequential like tar, and fsimg should expose filesystem object identities rather than forcing a serialized archive entry model.

### 17.1 Internal modules

```text
fsimg/core/io          random read/write, overflow-checked region, GPT, UDIF adapter
fsimg/apfs/probe        raw container probe, NXSB, checksums, flags
fsimg/apfs/checkpoint  descriptor/data rings and ephemeral object mapping
fsimg/apfs/object      typed header/read, storage class, cache, XID context
fsimg/apfs/omap        OMAP predecessor/range lookups and tombstones
fsimg/apfs/btree       generic B-tree nodes, tree-specific comparators
fsimg/apfs/volume      APSB, volume role/name/group, tree handles
fsimg/apfs/fsrecord    inode, drec, xattr, stream, sibling, snapshot key/value codecs
fsimg/apfs/stream      logical extent reader, sparse holes, crypto hook
fsimg/apfs/compress    decmpfs envelope, resource fork chunks, codec adapters
fsimg/apfs/ssv         fext tree, digest verification, seal status
fsimg/apfs/spaceman    allocator parser and (later) writer model
fsimg/apfs/write       logical namespace builder, packer, checkpoint emission
fsimg/apfs/validate    semantic constraint checks and diagnostic graph
fsimg/tests/fixtures   copyright-safe locally generated Apple/BSD-format images
```

No APFS-specific on-disk `struct` should be directly aliased over untrusted bytes. Use explicit endian reads, checked bounds and typed wrappers so host structure padding, alignment and 32-bit assumptions cannot corrupt decoding.

### 17.2 Public-facing capabilities

A possible thin API (names illustrative, independently proposed):

```text
apfs_probe(reader_at) -> {offset, block_size, is_apfs, feature_mask}
apfs_open(reader_at, opts) -> Container
Container.volumes() -> [VolumeInfo]
Container.open_volume(uuid_or_index) -> Volume
Volume.stat(path, follow_symlink=false) -> EntryInfo
Volume.list(path) -> iterator<EntryInfo>
Volume.open_file(path) -> ReaderAt             # random access, logical bytes
Volume.readlink(path) -> bytes
Volume.list_xattrs(path) -> [name]
Volume.get_xattr(path, name) -> bytes
Volume.snapshots() -> [SnapshotInfo]
Volume.open_snapshot(name_or_xid) -> Volume     # immutable view
apfs_create(raw_writer_at, input_tree, profile) -> Result
```

Suggested options: `verify_object_checksums=true`, `validate_btrees=true`, `verify_seal=false`, `checkpoint=latest|xid`, `recovery=strict|fallback`, `volume=<uuid|index>`, `max_decompress_bytes`, `max_tree_depth`, `max_metadata_nodes`, `metadata_mode=best_effort|strict`, `symlinks=preserve|reject`, `name_collision=fail|escape`. Expose unsupported file attributes as typed metadata rather than inventing fake POSIX interpretation.

### 17.3 CLI ergonomics

```sh
fsimg info disk.raw                            # list container + volumes + flags
fsimg tree --volume 0 disk.raw                  # directory listing
fsimg extract --volume 0 disk.raw outdir/       # preserve metadata when possible
fsimg extract --snapshot NAME disk.raw outdir/  # selected historic state
fsimg cat --volume UUID disk.raw /path/to/file
fsimg verify --deep disk.raw                    # all B-trees / extents / checksums
fsimg create --fs apfs --size 512M srcdir/ out.apfs
fsimg create --fs apfs --gpt --size 512M srcdir/ out.img
```

Distinguish generic `fsimg verify` from native `fsck_apfs`. An internal validator may not understand every latest proprietary implementation invariant. W0 profiles should print compatibility flags and supported feature decisions, not silently downgrade a snapshot/clone/ACL request.

## 18. Prioritized engineering work packages and acceptance gates

### R0 — Container navigation

Deliver: raw/GPT disk probing, little-endian object decoders, checksums, NXSB, feature reporting, XID checkpoint selection, ephemeral map, container OMAP, volume listing. Tests: 10 Apple-created DMGs / converted raw containers, malformed `NXSB` and rings, minimum and moderate container sizes. Done when fsimg reports every volume UUID, role, name and block count matching native `diskutil`.

### R1 — B-tree and ordinary extraction

Deliver: generic B-tree node reader, OMAP predecessor, FS key comparators, inode/xfields, directory records, file extents, sparse stream, symlinks. Tests: empty/one/large directory, deep tree, many fragmented extents, long names, small/huge sparse files. Done when SHA-256 content and path trees agree with macOS for all fixtures.

### R2 — Fidelity and robustness

Deliver: xattr streams, hardlink siblings, shared-clone extent reads, nanosecond metadata, case-sensitive and case-insensitive comparison, multi-volume tree separation. Tests: multiple hardlinks to one inode, `cp -c` plus overwrite, 2 volumes sharing the container, Unicode normalization collision tests. Done when metadata manifest matches and all invalid offset variants terminate safely.

### R3 — Advanced readers

Deliver: named snapshots, compressed decmpfs inline/resource fork codecs, sealed `fext_tree`, selected hash checking, optional FileVault software path. Tests: saved Apple snapshot fixture, transparent compression across codec types, sealed System/Data volume groups. Done with independent reader agreement and accurately labelled partial feature support.

### W0 — Minimal creator

Deliver: bottom-up new-image serializer with 4 KiB blocks, deterministic allocator, spaceman, OMAP, APSB, FS tree, extents, complete checkpoint. Output one unencrypted APFS container with a simple directory tree. **Pass/fail is native macOS mount + ordinary write + `fsck_apfs -n` clean**, not just fsimg self-open. Fix failing metadata by comparing small *facts* and generated fixtures against Apple's native creator and BSD-licensed implementation, not by copying external non-BSD code.

### W1 — Featureful creator

Deliver: xattrs, hardlinks, multiple volumes, sparse stream encoding, case-sensitive profile, optional GPT wrapper, larger/multilevel B-tree writer and reproducible byte output. Done after 10,000+ file fixture plus native post-mount modifications, checksum and spaceman validation. Do not implement preexisting-image mutation just because W1 can write a complete new image.

### W2 — Advanced writer

Only after W0/W1 robustness: clone COW, snapshots, correct delayed reclamation, offline append transaction, compressed write, allocator packing and size reduction, direct updates. Make each optional.

## 19. Unresolved questions / mandatory fixture experiments

The following items are intentionally **not** claimed to be completely specified here; they require fresh native fixture experiments or per-version feature decisions:

1. **New macOS formats after June 2020.** Apple's published 2020 PDF predates later APFS changes; test recent macOS Sequoia/Tahoe-era-produced volumes, Time Machine and installer layouts instead of assuming all fields unchanged.
2. **Checkpoint descriptor indirect/noncontiguous layout.** Fully support the high-bit encoding and map hierarchy before claiming arbitrary container compatibility.
3. **All native `newfs_apfs` object defaults.** Writer must match special inode/private-dir setup, free-queue and reaper initialization accepted by current `fsck_apfs`.
4. **Hashed filename exact test vectors.** Character normalization version, fold behavior, terminal-NUL CRC-32C convention and key comparator need fixtures in both case modes.
5. **Root B-tree payload geometry.** Confirm original tree metadata offsets, variable/fixed TOC, no-header and hash-footers with test images and controlled COW transitions.
6. **Snapshot variants.** macOS Time Machine, local snapshots, snapshot metadata extensions and dataless variants do not all have identical feature profiles.
7. **Compression resource-fork layouts.** Check every chosen `decmpfs` type with both inline and chunked payloads, multiple chunk counts and passthrough blocks.
8. **Sealed System Data lookup.** Verify sealed extent tree and digest semantics with actual boot snapshots; do not claim authenticity based solely on APFS checksum.
9. **Keybags and hardware encryption.** Software FileVault may be independently testable; T2/Secure Enclave keys and per-file protection often are not available from a disk image alone.
10. **Fusion.** Two-store physical address decoding and allocation zones must be implemented explicitly before accepting Fusion images.
11. **Writer transaction durability.** A native mount that works once is not evidence of correct crash consistency; power-cut tests are required for in-place modification.
12. **Extra metadata.** Finder resource forks, ACLs, quarantine flags, BSD flags, firmlinks and special files need explicit portability/lossiness policies.
13. **Interoperability licensing.** Reassess individual source-file licenses whenever importing any code or update from the listed repositories, even BSD-labelled ones.

## 20. Fast start: the five most valuable actions

1. Obtain three golden inputs: an Apple-formatted **raw** APFS container; a native **UDRW DMG/GPT** containing APFS; and a multi-volume Apple-created APFS container with mixed directory/xattr/clone tests. Record image layers exactly.
2. Implement R0+R1 based on Apple's PDF with standalone B-tree + OMAP parsers; use the BSD-3 Go reader and `apfs-fuse` as independent comparisons, preserving the source-code license boundary.
3. Build the manifest-based test harness before W0: verify content and metadata against macOS mounted image, then against external Linux oracles.
4. Prototype W0 by producing a **new empty image** and aiming for clean `fsck_apfs -n`; then add one file, a directory, and gradually deeper/multileaf trees. Do not tackle snapshots or encryption in the initial writer.
5. Add checksum-repaired semantic fuzzing and APFS record unit fuzzers early; a corrupted image is hostile input, and bounds and XID logic are far easier to validate before code paths proliferate.

---

**Research provenance and reuse policy.** This is an independently authored technical design, with cited public facts about APFS's on-disk wire format, original pseudocode and original testing proposals. It does not embed copied GPL/LGPL/MIT/Apache/APSL/proprietary implementation source. Links to such implementations are provided for reading, running as **separate external programs** and independently validating behavior, not for incorporating their code into fsimg. All code-reuse candidates require a fresh license, third-party component and dependency audit before implementation. The specification cannot guarantee full interoperability until fsimg-generated images are exercised against native macOS and independent validators.
