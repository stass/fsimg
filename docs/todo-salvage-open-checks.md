<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com> -->

# Salvage opening and preparation checks

Open work for the [salvage design](../specs/design-salvage.md). Opening stays
strict; this survey identifies the recovery that a separate relaxation policy
can buy. References identify refusal sites and their dependencies, not evidence
of damage frequencies in a corpus. Each open refusal below locks out the whole
selected filesystem. A preparation refusal locks out that directory's entire
child listing under salvage, the whole namespace when it is the root.

The classification is:

* **(a) Required:** the supported decoder needs this fact to establish the
  volume or the dependent structure's bounds, interpretation or authority.
  Never accept an invalid value. Where the dependency is local, unrelated
  structures can still be recovered by isolating that dependency.
* **(b) Relaxable:** a narrower, explicit salvage rule can recover specified
  data without accepting the failed structure as valid. The row states that
  rule and its limitations; it is work to design, not permission to ignore an
  error code.
* **(c) Irrelevant to extraction:** information or verification facts that no
  extracted path or payload depends on. If open enforces them, they can reject
  the whole image needlessly; operational failures still terminate.

The ordinary truncated-input contract permits all six families to open with
regular-file payload past the slice. Required metadata that sizes resources
must be present before those resources are sized, and every
truncated image fails full verification. Missing unused tails do not prevent
ordinary extraction. FAT's metadata end, exFAT's heap start/FAT, IFS's record
table, QNX4's bitmap-based truncated geometry and QNX6's proven-truncation
first-header path are not proposed salvage relaxations. Salvage's pre-creation
plan uses the existing append-only extent index and stopped mark: keep the
recoverable prefix in a shortened `.partial` file without following later
ranges or branches. ISO sections clip at the readable whole-block domain;
the first clipped section ends the prefix. These payload cases need no
open-check relaxation. Source references
identify validation responsibilities under that
contract. Probe identity has its ordinary explicit `--force` waiver; salvage
adds none.

Each name is handled independently, including names sharing an object or
storage. Existing limits apply; `max_work` covers opening and extraction
together, with no salvage-specific allowance.

Checksum-covered ancillary fields still meet their checksum gate: ignoring a
free counter does not rescue a header whose CRC is also invalid.

## Recovery priorities

* [Medium] **Reduce global refusal caused by local damage** (`lib/formats/`):
  ImageFS has the broadest namespace dependency: every record, even one later
  shadowed, is decoded and range-checked before any traversal. One bad ordinary
  file record can hide every otherwise readable file. exFAT similarly frames
  every root entry set to discover system metadata; QNX4 and QNX6 instead open
  their system structures and inspect ordinary records as directories prepare.
  FAT has the narrowest open pass. These are structural comparisons, not a
  measured ranking of recovery rates.
* [Medium] **Evaluate one valid QNX6 header independently**
  (`qnx6/header.c:97`, `qnx6/header.c:191`, `qnx6/header.c:249`): accepting the
  sole fully validated header when its peer is corrupt can recover an entire
  committed snapshot, even when one damaged header sector is the only fault.
  This is the largest single structural recovery opportunity: all files of
  the surviving snapshot, subject to their own checks. exFAT's one-valid-boot
  rule has the same whole-volume upper bound. Neither is a measured win over
  the other. QNX6 needs an explicit snapshot-age/provenance report and must
  not retry an older tree after a selected tree fails. Reject two invalid
  headers and retain domain, bitmap and resource checks. The ordinary
  proven-truncation exception already covers a missing second header, not a
  present peer with a bad CRC.
* [Medium] **Separate the simplest unnecessary gates first**
  (`iso9660/eltorito.c:325`, `qnx4/volume.c:419`, `ifs/volume.c:197`): El Torito
  validation and QNX4 `lost+found` checks have no exposed-content dependency;
  IFS's declared-domain file-range check can move to entry recovery. This
  concerns corrupt offsets/sizes, not valid payload ranges past a cut, which
  salvage retains only up to the first missing byte on openable images. These are
  smaller changes than partial system-tree recovery, but still need explicit
  loss reporting and regression fixtures. No relaxation changes verification.

## FAT12/16/32

* [Medium] **Separate FAT geometry, identity and optional fields**
  (`lib/formats/fat/`): retain the allocation interpretation while allowing
  independently framed damaged children to stop blocking their siblings.

| Check and source | Class | Damage, lockout and possible recovery |
| --- | --- | --- |
| Boot record presence; legal sector/cluster sizes, nonzero reserved/table counts and table size; ordered regions, cluster-count type, FAT capacity and representable identifiers (`codec.c:116`) | (a) | Torn BPB or corrupted count/shift prevents trustworthy offsets, chain width or resource bounds; whole volume unavailable without independent geometry. No BPB guessing. The truncated-input rule requires metadata present, not the whole data tail. |
| FAT12/16 fixed-root/FAT relations; FAT32 root range and active-table selection (`codec.c:204`) | (a) | Corrupt root or active FAT destroys the only selected namespace/allocation interpretation. A valid alternative root/table is a separate recovery source, not a waived bound. |
| FAT count above two, FAT32 nonzero revision and reserved extension bytes (`codec.c:199`, `codec.c:215`, `codec.c:235`) | (a) | May be valid unsupported media or damaged fields. These are decoder/profile gates, not evidence that bytes have ordinary semantics. Additional-table support is separate planned work. |
| Forced FAT variant disagrees with cluster-count type (`read.c:172`) | (a) | Wrong user format or corrupt cluster count; using the forced width can misdecode every chain. Use the correctly identified variant, not salvage reinterpretation. |
| Boot voucher, FAT media echo and reserved EOC entry (`read.c:198`, `read.c:212`, `read.c:232`) | (b) | Boot-sector/reserved-entry damage can hide an intact namespace. Existing explicit identity assumption already waives these identities while leaving geometry strict; no additional salvage waiver is needed. |
| Mirrors disagree on reserved entries (`read.c:222`) | (b) | One corrupted reserved slot rejects all files although no file chain uses slots 0/1. Restrict mirror comparisons to links actually used for salvage; keep used-link disagreements as damage, never arbitrarily choose a copy. |
| FAT32 FSInfo/backup-sector locator validity in the combined root check (`codec.c:229`) | (c) | Damaged advisory/free-count or backup locator rejects all files although extraction never reads those structures. Split from indispensable root/active-table validation; report the discarded advisory fact. |
| Extended boot label decoding (`read.c:253`, `codec.c:490`) | (c) | A bad label byte can reject all files; omit the label instead. Dirty/I/O flags are already info, not open refusals. |
| Prepare: chain bounds, cycle/shared-directory claims, slot/LFN sequence/count/checksum/UTF-16, live-entry attributes/EA/size/first cluster/timestamps, root/directory labels and common name/alias uniqueness (`directory.c:94`, `directory.c:205`, `directory.c:231`; `codec.c:269`, `codec.c:411`; `lib/read/directory.c:392`) | (b) | A torn 32-byte slot, orphan LFN, invalid time or chain link loses the whole listing; a bad root listing hides everything. Independent slot groups could permit entry omission, optional-time loss or a safe short-name policy, but orphan framing and aliases need explicit rules. Invalid names never become paths; size/domain/termination checks remain required for the affected object. |
| Dot-record consistency and full ownership reconciliation (`directory.c:60`, `verify.c`) | (c) | Dot validation is verification-only in FAT, so damaged dots alone do not gate extraction. Allocation ownership and unused mirror differences likewise must not be described as an extraction preflight. |

## exFAT

* [Medium] **Decouple system discovery from unrelated root entry sets**
  (`lib/formats/exfat/`): recover ordinary files only under an established
  heap and a trustworthy name/allocation policy.

| Check and source | Class | Damage, lockout and possible recovery |
| --- | --- | --- |
| Boot identity in each region (`boot.c:39`), probe signature/legacy zeros (`descriptor.c:59`) | (b) | Corrupt jump/name/signature can conceal intact geometry; identity assumption already covers open's identity checks. Probe zeros are also validated at open. Salvage does not imply assumption. |
| Sector/cluster shifts, checked volume arithmetic, metadata/heap ordering, count/root ranges, heap and FAT capacity (`boot.c:48`, `boot.c:99`) | (a) | Corrupt shifts/offsets destroy all addressing and resource bounds. Retain declared minimum volume size and required metadata presence under the truncated contract; no actual-slice minimum that rejects an otherwise usable truncated volume. |
| Twelve-sector checksums, extended boot signatures, and agreement of both valid geometries (`boot.c:57`, `boot.c:125`) | (b) | A torn primary/backup boot region or checksum sector rejects the whole volume. One independently valid region could establish geometry, with explicit degraded-redundancy reporting; conflicting valid regions need a selection policy, not a checksum waiver. |
| MustBeZero bytes; legal revision/FAT count and active-allocation flags; unsupported TexFAT/revision (`boot.c:74`) | (a) | Nonzero extension fields, a second FAT or a new revision may change semantics. Do not pretend this is the supported one-FAT profile. Known advisory fields are separated below. |
| PercentInUse outside 0..100/255 (`boot.c:89`) | (c) | A stale/damaged utilization byte rejects all files; traversal uses bitmap bits, not this percentage. Dirty/error flags already report rather than refuse. |
| FAT[0] media descriptor (`read.c:262`) | (b) | Reserved-entry corruption alone locks out all files. Ignore this unused reserved entry only under an explicit degraded-allocation identity policy; keep all used entries checked. |
| Root chain indexing, valid cluster links, bitmap/upcase record framing/uniqueness/presence/lengths (`read.c:40`, `read.c:266`; `allocation.c:91`) | (a) | Without a recoverable root stream and unique system locators there is no namespace or system metadata authority. A broken root tail could allow bounded discovery from its intact prefix, but requires a separate completeness/duplicate policy; accepting arbitrary locators is unsafe. |
| All root entry-set framing/checksums, entry after terminator, orphan secondary/type 0x80 and unknown critical entries during discovery (`read.c:63`, `directory.c:61`) | (b) | One ordinary file's damaged set checksum/secondary count prevents every extraction before root prepare. A dedicated bounded system-entry discovery pass could ignore a correctly framed unrelated file set and recover other files. A lost frame stops the remainder; do not scan for a plausible system entry. Unknown critical system semantics remain unsupported. |
| Label count, characters and UTF-16 (`read.c:92`) | (c) | Duplicate/bad label hides the entire volume despite having no filename role; discard label info, retaining system-entry uniqueness. |
| Bitmap/root/upcase allocation checks; upcase encoded length, identity runs, complete 65,536 mappings, checksum and mandatory ASCII mappings (`read.c:120`, `read.c:146`; `allocation.c:242`) | (b) | Local bitmap/upcase damage locks out all files. Independently established allocation/remaining mappings could recover unaffected names; a missing upcase table requires an explicit raw-name/collision policy, not an implicit Unicode replacement. Broken system locators/bounds remain (a). |
| Prepare: complete entry sets, attributes/timestamps, stream size/ValidDataLength/first-cluster relations, UTF-16/name hash, folded uniqueness, directory allocation/claims (`directory.c:152`, `directory.c:280`; `codec.c:106`, `codec.c:129`, `codec.c:156`; `lib/read/directory.c:392`) | (b) | A bad 0x85 set, name hash, stream field or directory cluster loses the listing/subtree; the root hides all files. Independently framed sets could be omitted. Never copy bytes past valid initialized length or through an invalid domain. |
| Prepare: root-only system entries, benign/vendor allocation flags and referenced streams (`directory.c:85`, `directory.c:123`, `directory.c:152`) | (b) | A misplaced system entry or bad allocation in an otherwise ignorable vendor set loses the listing. Quarantining an independently framed set can preserve ordinary files; do not treat unknown allocation ownership as proof that its storage belongs to another file. |
| Unreferenced allocation, free counts and full ownership (`verify.c`) | (c) | Verification work, not open/prepare prerequisites; ordinary file/file cross-links are not a global extraction ownership check. |

## ISO 9660

* [Medium] **Keep the chosen hierarchy independent of boot/info structures**
  (`lib/formats/iso9660/`): the directory tree can survive damage to structures
  only booting or verification uses.

| Check and source | Class | Damage, lockout and possible recovery |
| --- | --- | --- |
| Readable descriptor blocks, supported type/version framing, present primary, bounded scan/terminator location (`volume.c:99`; `codec.c:406`) | (a) | Damage to the only usable primary geometry/root or loss of framing prevents establishing the volume. The descriptor work bound stays; recognizing a primary prefix without a terminator needs an independently justified content boundary. |
| Later descriptor CD001, differing duplicate primary, multiple Joliet descriptors, terminator zero padding (`volume.c:126`, `volume.c:146`; `codec.c:444`) | (b) | A bad redundant/unselected descriptor or padding byte rejects all hierarchies. A separately selected valid descriptor and known descriptor-set end could recover its tree; never silently select between conflicting primaries. Block-16 identity retains its existing explicit waiver. |
| Primary both-endian geometry, supported block size/volume-set interpretation, selected root record framing/kind/extent/length (`codec.c:285`, `codec.c:309`, `codec.c:331`; `volume.c:178`) | (a) | Corrupt geometry or the only selected root destroys a safe namespace starting point. Do not choose one contradictory endian size or accept multi-volume references as local data. Root date/padding fields can be separated from its locator. |
| Path-table size copies/locations and bounds (`codec.c:338`; `volume.c:182`) | (c) | Corrupted unused path tables or their descriptor pointers hide every file. Extraction walks directories, never path tables; defer these validations to verification. |
| Joliet descriptor decode, flags, geometry agreement and root checks even with an explicit primary view (`codec.c:429`; `volume.c:216`) | (b) | A torn supplementary descriptor rejects a sound primary/Rock Ridge tree. Validate only the independently selected hierarchy for salvage, retaining primary geometry as the domain authority. Do not switch a selected Joliet view mid-walk. |
| File structure version and selected directory feature interpretation (`codec.c:342`, `codec.c:460`) | (a) | Unrecognized structure version/interleaving/record format changes interpretation. Additional decoders are profile work, not damage waivers. |
| Boot descriptor uniqueness/fields; catalog location, validation checksum/key/platform, entry flags/media/section framing and one-block limit (`eltorito.c:136`, `eltorito.c:156`, `eltorito.c:192`, `eltorito.c:238`, `eltorito.c:268`, `eltorito.c:325`) | (c) | A damaged or merely unsupported El Torito catalog hides all ordinary files. Skip boot summary parsing for extraction or report it unavailable; no hierarchy depends on it. This includes appended boot-image placement facts. |
| Auto/Rock Ridge handshake: root dot record, SP/ER framing, SUSP continuations and RRIP version/skip support (`volume.c:260`; `susp.c:259`) | (b) | Damage in root system-use bytes can reject auto opening despite a usable Joliet/primary tree. Explicit primary/Joliet already bypasses this handshake. A salvage-auto fallback needs a visible chosen-view/lost-metadata policy; required Rock Ridge never silently changes views. |
| System-area usage, labels/descriptive fields and malformed descriptor dates (`volume.c:237`; `codec.c:351`) | (c) | The system area has no byte-validity refusal; read failure still terminates. Bad descriptor dates already become absent and fail verification, not open. Do not add an extraction integrity gate for them. |
| Prepare: whole-block records/padding, dot/parent records, multi-extent ordering/length/limits, selected names, SUSP/RR NM/PX/TF/SL and CL/PL relocation, child-directory bounds and common uniqueness (`directory.c:104`, `directory.c:174`, `directory.c:255`, `directory.c:354`, `directory.c:533`; `rockridge.c`) | (b) | One corrupt record, time or continuation loses its listing, at the root everything. Independent framed records could survive with an omission policy; a damaged continuation loses its owning entry. A valid regular-file section crossing the readable content-domain end stops salvage's prefix; that section's unavailable tail and all later content are omitted. This is not a listing refusal or opening relaxation. Relocation ambiguity or lost framing never authorizes guessed descent. Unsupported critical storage features stay (a). |

## ImageFS (ifs)

* [Medium] **Isolate framed catalog records before hierarchy construction**
  (`lib/formats/ifs/`): this is the widest ordinary-file validation pass and
  offers recovery from a single damaged record without weakening host paths.

| Check and source | Class | Damage, lockout and possible recovery |
| --- | --- | --- |
| Header presence/signature, recognized byte-order flags, ordered header/table/image bounds and required table bytes (`volume.c:76`; `records.c:193`, `records.c:237`) | (a) | Corrupt byte order or table bounds prevents safe catalog interpretation. Signature alone has the ordinary identity waiver. The declared image may exceed the slice under the truncated contract; table bounds still precede resource sizing. |
| Image-size multiple of four; table alignment and spare header words (`records.c:246`; `volume.c:107`) | (b) | Padding/spare-word damage refuses every file. With independently valid table/data bounds and a recognized layout, extraction could ignore checksum alignment/padding facts; unknown semantic flags stay unsupported. |
| Mountpoint terminator/1024-byte limit and exact placement before the table (`volume.c:137`) | (c) | A damaged mount string or padding relation hides all files although extraction paths are table-relative. Use the validated dir_offset for framing and omit mountpoint info. Invalid mountpoint encoding already reads as absent. |
| Record length/minimum/bounded end/alignment, final zero terminator (`volume.c:219`, `volume.c:273`; `records.c:403`) | (b) | A torn length loses framing for the remaining catalog and globally rejects all files. A bounded prefix-recovery policy could retain independently framed records before it; never search payload for the next plausible record. A damaged final terminator need not invalidate all prior framed records under that policy. |
| Mode/kind/EA/inode-word/flag-nibble decoding; per-kind path/target framing and length, FIFO zero words (`records.c:279`, `records.c:333`, `records.c` `record_link`) | (b) | One unsupported or damaged record, even shadowed, refuses the entire catalog. With trustworthy outer length, omit/quarantine the record; preserve its pathname shadowing effect when that path is knowable, so an older duplicate is not resurrected. Do not assume unknown flags leave a usable object. |
| Absolute, empty or dot path components; empty path must be a directory (`names.c:20`; `tree.c:381`) | (b) | One unsafe or out-of-model path blocks all files. Omit that record/subtree while keeping other safe paths; never sanitize it into a host path or expose children through a discarded non-directory ancestor. Names outside the portable profile already become omissions, not open failures. |
| Every nonempty regular file lies in `[hdr_dir_size, image_size - 4)` (`volume.c:197`, `volume.c:246`) | (b) | A flipped offset/size outside the declared data region blocks all files, including shadowed records. Moving this check to entry planning permits abandoning that entry. An in-domain range beyond the slice passes open under the truncated contract: salvage keeps its present prefix in a shortened `.partial` file and continues siblings. On an openable IFS image this is the reachable content-damage case; scratch faults are terminal and unchecked payload changes are not detectable damage. |
| Sort/child-stream/group-table integrity and preorder/tree-order invariants (`tree.c:150`, `tree.c:284`, `tree.c:412`, `directory.c:82`) | (a) | These are scratch/internal contracts, not recoverable image damage. Ignoring them can assign a child to the wrong parent. Duplicate paths and non-directory ancestors already have defined last-record/shadowing semantics. |
| Whole-image checksum, boot/script reference matching, trailing-volume agreement (`verify.c:53`, `verify.c:85`, `verify.c:145`) | (c) | Verification-only; extraction does not preflight checksum or boot references. Their damage alone does not lock out an openable image. A truncated image still fails core full verification. |

## QNX4

* [Medium] **Decouple hidden/system objects from independent direct records**
  (`lib/formats/qnx4/`): root and direct files need fewer services than linked
  inodes and long names do.

| Check and source | Class | Damage, lockout and possible recovery |
| --- | --- | --- |
| Geometry/root/bitmap placement, block-count bounds and bitmap-derived truncation inference (`volume.c:64`, `volume.c:122`) | (a) | Corrupt root or bitmap sizing destroys domain/resource authority. The ordinary truncated-image rule supplies geometry where bitmap size proves it; an arbitrary longer slice or guessed bitmap placement is not salvage evidence. |
| Root identity and record-0 directory locator/kind/size/extent validity (`volume.c:150`; `inode.c:66`) | (a) | A corrupt sole root locator leaves no namespace without carving. Identity bytes alone retain the ordinary explicit waiver; structural checks remain. |
| All five root-block records' names/status/kinds/size/extent relations and reserved-zero ranges (`volume.c:158`, `volume.c:168`; `descriptor.c:114`) | (b) | A torn `.boot`, `.altboot`, `.inodes` or `lost+found` record blocks even unrelated direct files. Validate root authority independently and quarantine the damaged service/object. Unknown root semantic extensions require separate evidence. |
| Bitmap accessibility, loader/root/self-allocation bits and tail/padding ones (`bitmap.c:90`) | (b) | A flipped self/padding bit hides every file; a lost bitmap block prevents dependent allocation checks. Self/padding facts can be degraded without authorizing unallocated content. Partial bitmap availability needs explicit unknown-bit policy and system reservations; its bounds/presence for resource sizing remain (a). |
| Root scan allocation/chain; fixed dot/bitmap/system-link slots; duplicate format-owned names and `.longfilenames` discovery (`volume.c:190`, `volume.c:295`, `volume.c:336`) | (b) | A bad reserved slot, root extent chain or duplicate system locator rejects all files. Root stream damage could expose a validated prefix; independent system discovery must quarantine ambiguous services, never choose a duplicate locator. Actual namespace framing/host-name checks stay required. |
| `.inodes` size, extent allocation/exclusive claims, signature and locator (`volume.c:252`) | (b) | A damaged inode-table signature or chain blocks all direct files too. Retain independent direct records and disable links needing the unavailable table, with explicit losses. Do not resolve a link outside verified table extents. |
| Optional `.longfilenames` record, kind/whole-record size, allocation/claims and locator (`volume.c:357`) | (b) | One damaged name-table chain rejects all short names. Recover direct/short names independently; long-name links become unavailable, not silently renamed to a prefix. |
| `lost+found` allocation/claim/slot framing and dot/parent targets (`volume.c:401`, `volume.c:419`) | (c) | A hidden recovery directory's damaged chain/dots rejects every exposed file. Reserve its valid known storage, report uncertain ownership and omit its interpretation. Used slots already do not block open and are verification findings; no lost+found carving is implied. |
| Prepare: complete allocation/claim checks; slot status, dots, child inode/status/size, link-table resolution, long-name pointer/backreference/prefix, common uniqueness (`directory.c:53`, `directory.c:115`, `directory.c:180`, `directory.c:310`; `inode.c:129`; `names.c:115`) | (b) | A single bad link/long-name block or dot target loses the listing; a root fault hides all exposed files. Fixed 64-byte framing permits an omission design, but target authority, namespace and independent safe object-size checks must survive. Unknown kinds/extensions stay unsupported. |
| Label validity; directory link counts, full bitmap ownership/free-count reconciliation (`volume.c:99`; `verify.c`) | (c) | Invalid labels already become absent. Directory link-count checks run only with verification references; they are not extraction gates. Full ownership and hidden used lost+found entries remain verification findings. |

## QNX6

* [Medium] **Separate snapshot redundancy and optional naming from core authority**
  (`lib/formats/qnx6/`): do not make an intact committed tree depend on an
  unreadable peer header or an unrelated long-name branch.

| Check and source | Class | Damage, lockout and possible recovery |
| --- | --- | --- |
| Classic layout/minimum readable header, supported version/block size/features/extensions; at least one CRC-valid header with unambiguous order (`header.c:69`, `header.c:97`, `header.c:227`) | (a) | Corrupt sole header or unknown format layout gives no safe decoder/domain. Missing magic alone has the explicit identity/CRC-order waiver. No checksum guessing, bootless/MMI fallback or unsupported-feature decoding. |
| Every candidate must validate, then pair identity/geometry agreement and equal-serial byte identity (`header.c:191`, `header.c:249`) | (b) | One torn sector or bad CRC in either otherwise redundant header locks out the whole volume. A sole fully valid candidate can recover its snapshot with an age/uncertainty report. Two valid disagreeing volumes/same-serial states need an explicit selection policy; no automatic choice after tree failure. The proven-truncation first-header path is already baseline behavior. |
| Valid block/inode counts, checked geometry and system reservation derived from bitmap/inode tree sizes (`header.c:149`, `header.c:171`) | (a) | Corrupt geometry/tree size can move the content boundary into system metadata or amplify resources. Preserve those relations; a different reservation derivation requires independent evidence, not a relaxed check. |
| Reserve percentage, groups, free counters; pair UUID/format-time facts (`header.c:149`, `header.c:195`) | (c) | Counter/identity damage can refuse all files, although lookup uses actual trees and allocation bits. Pair identity remains useful to select one volume; ignoring its disagreement without a selection policy is not justified by payload irrelevance. Free-counter reporting can simply become unavailable. |
| Native exact system-tree lengths, minimal depth/depth-two profile; names whole-block length/minimal depth (`header.c:35`, `header.c:167`) | (b) | Torn unused descriptor slots or nonnative padded/minimally shaped trees reject all files. With independently established reservation, bounded readable prefixes could recover tables; padded/nonminimal valid trees need profile evidence. Never shrink the reservation merely because a damaged system tree appears smaller. |
| Bitmap tree bounds/domain/required references/cycles, recorded self-allocation and tail ones (`bitmap.c:22`, `bitmap.c:58`, `bitmap.c:91`; `tree.c:211`) | (b) | A bad pointer, self bit or unused padding bit rejects all files. Self/padding checks can degrade independently; partial tree recovery can preserve known allocation ranges while dependent unknown ranges remain unavailable. System-domain and arithmetic checks remain (a). |
| Entire inode-tree index before root read (`read.c:88`; `tree.c:253`, `tree.c:299`) | (b) | A missing indirect block outside the root's inode branch rejects every file. Index independently framed branches and mark unavailable inode intervals; recover root and reachable valid inodes only. No resource sizing from absent metadata. |
| Entire long-name tree indexed/claimed before any directory (`read.c:105`) | (b) | Damage in an unused long-name branch hides all inline-name files. Preserve known table branches/claims and omit names depending on unavailable blocks; never substitute another name. Content/system domain boundaries remain fixed. |
| Root inode allocation/state/kind/size, tree depth/capacity and supported fields (`read.c:109`; `inode.c:20`) | (a) | A bad sole root record prevents starting a safe namespace. Separate optional metadata fields only with evidence that they cannot alter kind/tree semantics. |
| Prepare: directory tree claims, slots/dots, every live inode's state/kind/size/tree limits, inline/long-name lengths/checksums/extensions and common uniqueness (`directory.c:76`, `directory.c:107`, `directory.c:149`; `inode.c:20`; `names.c:48`) | (b) | A bad inode or name block loses every sibling in that listing; root damage hides the volume. Fixed 32-byte slots permit omission of independent entries, while bad directory allocation/framing still loses the listing. No invalid name or system pointer is used. |
| Link/reference counts, unused inodes, full ownership/free counts and payload verification (`verify.c`) | (c) | Verification-only work, not a traversal preflight. Mounted state already reports rather than refusing. Preserve that distinction. |

## Acceptance work

* [Medium] **Prove each relaxation recovers independent data**
  (`test/spec/`, format reader tests): pair a one-field/one-sector mutation
  with an intact sibling/snapshot and assert strict refusal, the proposed
  recovered set, exact losses, domain protection and finite work. Include
  conflicting valid copies, absent required metadata, shadowed IFS records,
  repeated names, source changes and scratch corruption. Use independent
  expected paths/bytes and the existing format oracles. A smaller refusal
  scope must not turn operational failures into image damage or make full
  verification pass. Gather corpus frequencies before making a quantitative
  claim that one format or relaxation benefits more users.
