# Open work

## Major initiatives

* [High] **QNX6 read/write and automotive qualification**
  (`lib/formats/qnx6/`, `lib/read/`, `lib/write/`): the classic reader,
  verifier and writer of the [Power-Safe design](specs/design-qnx6.md) are
  implemented through its [driver design](specs/design-qnx6-driver.md);
  close the explicit format-evidence gates, and qualify native read/write
  interoperability across automotive and extended variants. Porsche PCM 3.1 HDD reproduction also requires
  metadata fidelity and exact disk composition, including QNX4 where present.
  The [work breakdown](docs/todo-qnx6.md) owns experiments and implementation;
  [research evidence](docs/qnx6-research.md) owns source provenance.

* [High] **Tar-like CLI and image layers** (`cmd/fsimg/`, `lib/layers/`,
  `compat/`): implement the [command-line contract](specs/design-cli.md) and
  the [layer design](specs/design-layers.md) in the four stages of the
  [plan](docs/implementation-plan.md#extension-sequence). Containers and
  gzip are read and written, and `-f -` reads and writes streams; the
  command line recognizes and refuses with exit 1 the reserved zstd and xz
  compressions and `-M` minimization, which no stage schedules yet. The
  `hdiutil attach` gate of the container writer settles the layer design's
  DMG-table-split question.
  Stage 1 leaves these open:
  * The split staging, destination, scratch and root grants and the
    destination fixtures have run only on macOS, which has no sandbox
    backend; run `test-sandbox`, `test-sandbox-setup` and the CLI
    destination tests on Linux (Landlock) and FreeBSD (Capsicum). The raw
    intermediate and spool roles with their Capsicum rights, a stream
    creation's data channel, with its `CAP_FSTAT` right, and a stream
    read's input pipe have likewise run only unconfined
    (`cmd/fsimg/sandbox.c`).
  * `c -v` and `x -v` do not yet print one line per entry: the writer and
    extractor have no per-entry callback for the worker to relay
    (`lib/write/`, `lib/extract/`, `cmd/fsimg/worker-child.c`).
  * Tests do not yet feed the worker a malformed path sequence (a count
    differing from the record, a missing terminator, an extra or empty
    SELECTION frame), and no test drives a whole `cW` whose staged image
    fails verification: `test/worker.c` runs the verification job on a
    container corrupted after creation and checks the layer it names, and
    the display's attribution is tested with that error, but the command
    has no fault hook to corrupt its own staged file between the two
    workers (`cmd/fsimg/worker-child.c`, `test/worker.c`, `test/cli.py`).
  * [Low] The forged-worker tests cover no creation job, so the
    coordinator's checks of an encoded output's PLAN, of a created IMAGE
    record's compression against the job's, of DONE's staged length and of
    a stream's raw bytes against its plan run only against the real worker
    (`cmd/fsimg/worker-parent.c`, `test/worker.c`).
  * `x` and `t` accept one member; several members, the union of their
    subtrees resolved by each format's lookup rules, are refused with exit 1
    (`cmd/fsimg/options.c`, `lib/read/`).
  The layer read side leaves these open:
  * [Medium] The coordinator bounds default output by the disk length the
    worker reports for a DMG or sparse image, which it cannot check against
    the file; a compromised worker could raise it. A fixed VHD's disk is
    checked to be the file less its footer; a default ceiling for the
    other containers derived from the file length would take the rest of
    the trust out (`cmd/fsimg/worker-parent.c` `bound_volume`).
  The layer write side leaves these open:
  * [Medium] Hole knowledge tests lack the design's Sandbox and capability
    cases: no test spools and transcodes under the worker's installed
    Capsicum rights, none reads an external input limited without
    `CAP_FSYNC` (a discovery sync failing `ENOTCAPABLE`), and none reads an
    input on a filesystem without holes or exercises extraction against
    the discovery-sync counter (`test/sandbox.c`, `test/holes.c`,
    specs/design-holes.md Tests and evidence).
  * [Low] Whether a creation needs a raw intermediate is decided twice:
    the command line's `fsimg_job_needs_raw` (`cmd/fsimg/job.c`, which the
    coordinator and the worker share) and the library's
    `fsimg_disk_plan_encoded` with the sink's kind (`lib/layers/plan.c`,
    `lib/write/writer.c`). A public predicate over the stack, the write
    flags and the sink kind would let the command line ask the library.
  * [Low] The coordinator takes an encoded output's largest length (a
    container's or gzip's) from the worker's PLAN, checked only to exceed
    the disk, and a stream without `-l output` is bounded by it; a ceiling
    the coordinator derives from the request's layers and disk would
    remove that trust
    (`cmd/fsimg/worker-parent.c` `plan_frame_expected`). Likewise a fixed
    VHD's 512-byte footer is known to the coordinator by name
    (`read_disk_expected`) rather than from the layer registry.
  * [Low] An encoded output's sink is checked and sized to the largest
    encoding only after the whole raw disk is emitted, so a sink that
    cannot take a container or gzip fails late, and a host without sparse
    files allocates the bound until the sink is trimmed at close. gzip
    never leaves holes, so its sink could grow as it is written instead
    (`lib/write/writer.c` `output_through_raw`, `lib/layers/transcode.c`
    `sink_open`).
  * [Low] `cW` of a compressed image decodes it into the spool, so a disk
    larger than the spool limit fails verification only after the whole
    creation; the coordinator knows the planned disk from the IMAGE record
    and could refuse at planning (`cmd/fsimg/worker-parent.c`).
  * [Low] A layer's failure is attributed by `layer_failed` at each of its
    call sites in `lib/layers/image.c` and by the chunk index in
    `lib/layers/chunks.c`; a new layer entry point must remember to call
    it. The DMG encoder, the chunk index and gzip each carry their own
    zlib allocator over the context (`lib/layers/dmg.c`,
    `lib/layers/chunks.c`, `lib/layers/gzip.c`).
  * [Low] `fsimg_writer_output` holds the bare raw descriptor between the
    sink's adoption and its handover to `fsimg_io_adopt_handed_over`, so
    an early return added between them must close it; one helper that
    either returns the adopted object or has closed the descriptor would
    hold it by construction (`lib/write/writer.c`).
  * [Low] No oracle checks the MBR CHS fields fsimg writes (`sfdisk` and
    `fdisk` compare start, size and type), mtools never reads a FAT volume
    through a GPT disk, and unwrap is tested only to I/O sinks with the
    automatic and range selections (`test/layers-integration.sh.in`,
    `test/layers-write.c`).
  * [Low] When the worker dies without an error frame while the input relay
    is still blocked reading a slow standard input, the relay's channel
    closes by force and the command reports that child exit and channel
    closure did not complete, not that the worker failed
    (`cmd/fsimg/worker-parent.c` `supervise_operation`).

* [Medium] **QNX4 interoperability** (`lib/formats/qnx4/`): the
  [format design](specs/design-qnx4.md) and [driver design](specs/design-qnx4-driver.md)
  describe the reader, verifier and writer for native QNX4 volumes and the
  PCM 3.1 navigation partition, and all three are registered. The
  [work breakdown](docs/todo-qnx4.md) holds the open items: the Linux and
  qnxprobe comparisons and head-unit acceptance.
  [Research evidence](docs/qnx4-research.md) owns the native observations.

* [Medium] **Specification-driven testing** (`test/spec/`, `test/spec.sh.in`,
  `Makefile.am`, `.github/workflows/check.yml`): build the Ruby rule, case and
  runner harness. It derives valid and targetedly invalid FAT images from
  cited specification rules, checks fsimg's per-stage status against the
  outcome the rules derive, records independent tools' observations, and
  reports specification coverage. The
  [design](specs/design-spec-testing.md) owns the decisions and the
  [plan](docs/spec-testing-plan.md) the stages.

* [Medium] **Spec harness captures in memory** (`test/spec/lib/fsimg_spec/process.rb`,
  `outcome.rb`): each stage's stdout and stderr are held in Ruby strings up
  to the coordinator's text ceiling, which reaches several GiB under the
  `deep` profile, and stdout is parsed whole. Stream captures to their
  case-directory files and parse the NDJSON records line by line.

* [Medium] **Spec harness formatter oracles beyond mkfs.fat**
  (`test/spec/lib/fsimg_spec/fat/bases.rb`): formatter bases are built only
  with mkfs.fat and mtools, so a FreeBSD host without them runs on builder
  bases alone. Add the `newfs_msdos` formatter adapter, and read every base
  with fatcat as a secondary reader where `configure` found it.

* [Medium] **Spec harness field draws never reach fsimg**
  (`test/spec/suite/fat/test_rules.rb`, `test/spec/lib/fsimg_spec/fat/witnesses.rb`):
  PropCheck's BPB and geometry field draws are classified in memory only,
  while drawn trees run through fsimg in the profile's sample. Run the field
  draws through the same seeded sample, so about 30 drawn cases per variant
  reach fsimg under `quick`.

* [Medium] **Spec harness mutated images unclassified beyond `info`**
  (`test/spec/lib/fsimg_spec/fat/model.rb`, `test/spec/lib/fsimg_spec/fat/cases.rb`):
  the model classifies the directory, file and verification scopes only
  through a known tree, which an unmutated base and a created image carry;
  a mutated image leaves them unclassified, so its `ls`, `extract` and
  `verify` assert only safety. Milestone B2's table and directory decoder
  replaces the known tree and must decode those images to the same tree.
  The writer `profile` rule that every byte outside allocated metadata and
  file content is zero needs the decoder's allocation map and lands with it.

* [Low] **Spec harness FAT witness gaps**
  (`test/spec/lib/fsimg_spec/fat/witnesses.rb`):
  - `fat.geometry.max_cluster` has only a multi-rule witness, which also
    exceeds its image and FAT. Isolating it needs a sparse image of about
    128 GiB, above the `deep` profile's 64 GiB image ceiling.
  - Common-BPB field witnesses and draws vary the FAT12 base only; vary the
    FAT16 and FAT32 bases as well.
  - No witness puts the backup BPB sector past 29, where the builder can no
    longer render the three-sector record, so the accepted `BPB_BkBootSec`
    of 30 or 31 with 32 reserved sectors has no standing witness; add a
    mutation witness that places a backup BPB there.

* [Low] **Spec harness FAT rule wiring** (`test/spec/lib/fsimg_spec/fat/rules.rb`,
  `witnesses.rb`, `cases.rb`):
  - `Rules` names `Witnesses` domain constants and `Witnesses` reads
    `Rules::FIELDS`, keyed by rule id, to tell field rules from relational
    ones. Break the cycle, for example by making a rule's field part of the
    rule.
  - A case's recipe records the base's variant as its format, not the variant
    the mutated image decodes to (documented in `Cases#recipe`). Decide which
    one picks the format suite on replay.

* [Medium] **Promoted recipes on formatter bases** (`test/spec/lib/fsimg_spec/fat/suite.rb`,
  `fat/cases.rb`, `fat/bases.rb`): the design replays every promoted case
  whose builder the host has and skips the others, but only independent-builder
  bases replay: `Suite.promoted` refuses a promoted recipe on a formatter
  base, and `bin/replay` skips one. Replay them where the case phase's tool
  set has the formatter, with its recorded version.

* [Medium] **Killed worker once gave malformed NDJSON** (`cmd/fsimg/worker-parent.c`,
  `test/spec/suite/fat/test_runner_fsimg.rb`): in one full quick run on macOS,
  `test_killed_worker_is_a_fault` saw `ls` end as `harness_error` ("malformed
  NDJSON on stdout") instead of a fault record after its worker was killed.
  It did not recur in 40 runs of the test under CPU load or in 450 direct
  worker kills during `fsimg ls -j`. Either the relay can emit a partial
  record when a worker dies, which is an fsimg defect, or the test's killer
  raced; the assertion now quotes the end of stdout to tell them apart.

* [Medium] **Spec harness stage harness errors** (`test/spec/lib/fsimg_spec/outcome.rb`,
  `case_run.rb`): a stage's `harness_error` outcome covers both host faults
  (fsimg could not be launched, a sandbox status that differs from the
  preflight's) and fsimg's own protocol violations (malformed NDJSON, an exit
  status outside the contract). A case whose expectation such a stage fails is
  `failed`, so a host fault counts as an fsimg failure in coverage and is a
  bad revision to `bin/replay`. Split the host faults out so that they make the
  case a `harness_error`.

* [Low] **Spec harness reports and records** (`test/spec/lib/fsimg_spec/coverage.rb`,
  `failure_record.rb`):
  - Coverage reports `targeted_witnesses` as only `available` or
    `not_generated`, and has no field for the boundaries exercised; the
    design also names "impossible within the enumerated space". A promoted
    `valid` recipe counts toward no rule.
  - A promoted case that drifted is a `harness_error`, the verdict a
    generator defect or a base digest mismatch also gets; a distinct verdict
    or detail class would let the report tell a stale regression recipe
    from a harness fault.
  - A rule whose only cases could not be classified reports
    `predicate_only`, though no predicate ran for them; `witnesses` also
    lists promoted ids.
  - Failure records are bounded one by one (64 MiB logical image, capped
    captures) but not per run: a broken fsimg that fails every case writes
    one image per case, and the design's "when it is sparse" condition is
    not checked. Add a per-run budget for recorded images.

* [Low] **Scoped ownership intervals** (`lib/formats/fat/verify.c`,
  `lib/formats/exfat/verify.c`): extract the duplicate ownership-interval
  sorting and cursor checks into one bounded utility, the last item of the
  [consolidation plan](docs/implementation-plan.md#architecture-consolidation).
  Both verifiers keep their format-specific reconciliation.

## Bugs by subsystem

* [Medium] **FreeBSD fsck_msdosfs rejects the FAT16 builder base at 4085
  clusters** (`test/spec/suite/fat/test_bases.rb`,
  `test/spec/lib/fsimg_spec/fat/builder.rb`): on FreeBSD 16.0-CURRENT,
  `test_builder_bases_pass_the_checkers` fails at "fat16 at the threshold"
  with `fsck_msdosfs -n` exit 8: crossed chains near cluster 4080 and chains
  ending in free clusters, the pattern of 12-bit FAT decoding. macOS
  `fsck_msdos`, mtools and fatcat accept the image, and the 140 FAT cases pass
  on that host, so `test/spec.sh` fails there only on this base. Establish
  from the FreeBSD source whether its FAT12/FAT16 cutover differs from the
  specification's 4085; then either fix the builder or record the checker's
  deviation as a cited host-specific skip for that one base.

* [Medium] **Worker sanitizer exit after an error frame is lost**
  (`cmd/fsimg/worker-parent.c`): after the parser worker sends its final
  `FSIMG_FRAME_ERROR`, `finish_operation()` calls `cleanup_operation()`, which
  sends SIGTERM before the sanitizer exit status is examined. A sanitizer
  report or crash at worker exit is then killed or races with the signal, and
  the frame's verdict is reported instead of `FSIMG_ESTATE`. Wait a bounded time
  for the worker's own exit after a final frame before signalling it. Write a
  failing test first: a worker that sends an error frame and then exits with
  the reserved sanitizer status must yield `FSIMG_ESTATE`.

* [Medium] **Spilled metadata stores resize scratch on every growth**
  (`lib/meta/store.c`): once a store spills, each `fsimg_store_grow()`, one
  per appended record, runs `scratch_resize()`, whose `fsimg_io_resize()`
  calls `ftruncate(2)`. Sampling `./test-qnx6 --scale` on macOS while it
  lists a 200,000-file directory puts nearly all of the listing in that
  call, and the FAT scale case's 105-second listing likely shares the cause.
  Track a backing capacity and extend the file in larger sparse steps only
  when a growth exceeds it, while charging the logical length, so the
  scratch limit and physical-peak accounting stay exact.

* [Low] **Source failures fail the writer from inside the source layer**
  (`lib/source/source.c`, `specs/design-sources.md`): an operational source
  failure calls `fsimg_writer_fail()` and releases collection storage while a
  driver's emit call is still on the stack, whereas a sink failure during the
  same copy fails the writer only when `fsimg_writer_emit()` returns. Make the
  source cursor report status only and leave failing the writer to the writer
  lifecycle, so writer state after a failed copy no longer depends on which
  layer failed.
* [Low] **Monotonic-clock failures** (`cmd/fsimg/paths.c`,
  `cmd/fsimg/worker-parent.c`, `cmd/fsimg/main.c`): make later supervision,
  publication and reporting clock queries distinguish failure from zero.
  Initial deadline setup already returns `FSIMG_EIO`; later helpers can derive
  a reporting deadline or defer timeout detection after a failed query.
  Centralize the clock result and exercise failures without weakening bounded
  shutdown or adding a blocking diagnostic path.

* [Medium] **UFS publication handoff** (`lib/output/publish.c`,
  `test/publish.c --identity`, `test/extract.c`): investigate deferred ctime
  changes on FreeBSD 15.1/UFS after publication. Immediate readers can reject
  unchanged payloads with `FSIMG_ESOURCE_CHANGED`; inode fsync and an atomic
  no-replace-rename prototype do not establish a stable metadata baseline.
  Retain conservative identity checks and qualify the kernel bookkeeping before
  promising immediate publication-to-reader handoff on this filesystem.

* [Low] **Source-scanner diagnostics** (`lib/source/scan.c`, `compat/host.c`,
  `test/source.c`): a socket that fails during open is reported as host I/O
  failure instead of unsupported source kind. Add a socket regression and
  classify the failure without guessing object type from `ENXIO` alone.

* [Low] **POSIX metadata simplifications** (`lib/write/catalog.c`,
  `cmd/fsimg/protocol.c`, `test/extract.c`, `test/posix.c`): give the
  kind/size rule ("link 1..1023, directory and FIFO 0, file any") one
  internal owner shared by the catalog and the wire validator, so QNX6
  reuses it; share one all-zero-bytes check between them; make
  `test_unsupported_kinds` in `test/extract.c` a table over selection and
  skip flag; run the format-independent capability tests in `test/posix.c`
  once rather than per format; replace `images_equal` with a whole-file or
  digest comparison. About 70 lines, no behaviour change.

* [Low] **FAT volume-label escape** (`lib/formats/fat/codec.c`,
  `lib/formats/fat/directory.c`, `test/fat.c`): verify the directory-name
  first-byte `0x05` escape for a CP437 label beginning with `0xe5`, separately
  from the BPB label field. Add a failing root/BPB label fixture before
  changing the decoder's interpretation of either field.

* [Low] **Reserved FAT entries checked at open** (`lib/formats/fat/read.c`,
  `lib/formats/fat/verify.c`, `specs/design-fat-reader.md`): header-only
  open rejects a `FAT[0]` whose low byte differs from the BPB media byte and
  a `FAT[1]` without its end-of-chain bits. Neither divergence affects
  content. Move both checks into `verify`'s reserved-entry scope, so `info`,
  `ls` and `extract` accept them while full verification stays strict. This
  follows the
  [spec-testing stage policy](specs/design-spec-testing.md#stage-policy).
  Write the failing open/verify tests first.

## Security residuals

* [High] **Symbolic-link import under the Linux sandbox** (`cmd/fsimg/sandbox.c`,
  `lib/source/source.c`, `specs/design-security.md`): the source scan reads a
  link with `fstatat(AT_SYMLINK_NOFOLLOW)` and `readlinkat()`, which the
  seccomp allowlist denies because Landlock does not mediate path-based
  metadata queries. A confined Linux worker therefore fails a tree containing
  a link with `FSIMG_EUNSUPPORTED` ("symbolic link inspection is not
  permitted", naming the link) instead of importing it. Decide how a confined
  scan reads links: allowing both calls lets a compromised worker probe
  metadata and link targets outside the source tree; an `O_PATH | O_NOFOLLOW`
  open plus `readlinkat(fd, "")` needs `O_PATH`, which the profile refuses for
  the same reason; reading links in the coordinator moves source parsing out
  of confinement. Decision: the coordinator reads link targets. The worker
  sends the link's parent directory descriptor and a single name component
  over a request/reply channel; the coordinator checks the descriptor is a
  directory on the source device and the name is one component other than
  `.` and `..`, then inspects the link, reads the target (at most 1023
  bytes), inspects it again and returns target and identity. The library
  scanner reads links through an internal host hook that defaults to
  `readlinkat()` and that the Linux worker replaces. A walk of the image path
  from the source root is the alternative if descriptor passing proves
  awkward. Record the design in the security, CLI and metadata specs, then
  test it on Linux with the sandbox active, including forged worker requests.
  FreeBSD Capsicum permits both calls: a confined `create -S required` reads
  ordinary and dangling links (verified on FreeBSD 16.0-CURRENT).
* [Low] **Shared-library export list** (`Makefile.am`, `lib/`): libfsimg
  exports every external symbol, internal ones included, so a consumer that
  declares an internal function such as `fsimg_writer_new_variant()` itself
  can call it outside the public contract. Export only the public header's symbols
  (a libtool `-export-symbols-regex` or a symbol file, with test programs
  linking the convenience library for internals).

* [Low] **Coordinator trusts the reported compression** (`cmd/fsimg/worker-parent.c`
  `read_disk_expected`): a worker's IMAGE record naming a compression moves
  the coordinator's bound on the file from the input's exact length to the
  spool limit, so a compromised worker claiming gzip for a raw file widens
  the volume and output bounds the coordinator derives, up to the spool
  limit. The coordinator cannot check the claim without reading image
  bytes; the bound still holds the worker to the spool limit, as container
  disk lengths are already trusted.

## Planned features

* [Medium] **More than two FATs** (`lib/formats/fat/`, `cmd/fsimg/options.c`,
  `specs/design-fat-reader.md`, `specs/design-fat-writer.md`,
  `specs/design-sizing.md`): the Microsoft specification allows any nonzero
  `BPB_NumFATs`. The reader supports one or two and reports a larger count as
  `FSIMG_EUNSUPPORTED` (`FAT_READ_TABLES` also sizes the table window); the
  writer always emits two. Design per-copy mirror checks within the bounded
  window, FAT32's four-bit active-table field, a writer setting, sizing and
  the capability record; then implement both sides and check mtools and
  `fsck.fat` behaviour with more than two tables.

* [Medium] **Image minimization** (`lib/formats/fat/write-plan.c`,
  `lib/formats/exfat/write-plan.c`, `cmd/fsimg/options.c`): implement the
  [sizing design](specs/design-sizing.md): `FSIMG_WRITE_MINIMIZE` and `-M`,
  one layout pass accumulating every candidate unit's total, selection of the
  smallest volume, and the pinned test catalogs where it differs from the
  default, including the QNX6 search over the open block sizes and the exact
  inode capacity that its driver design specifies.

* [Medium] **Metadata round-trip through mtree** (`lib/extract/`,
  `lib/source/`, `cmd/fsimg/`): the safe extractor never restores owner, mode
  or change time and creates no links or FIFOs, so an extracted QNX6 tree
  cannot be rebuilt with its metadata. Write an mtree(5) spec beside the
  extracted tree on `x` and import one on `c`, through an option whose
  spelling the design decides (`-F` is the stack), on
  the model of FreeBSD `makefs -F` with a release `METALOG`: the host tree
  supplies bytes, the spec supplies `type`, `uid`, `gid`, `mode`, `time`,
  `link` and `sha256digest`, with no privileges. Plain mtree has only
  modification time, so access, change and creation times are counted
  losses rather than extension keywords that `mtree(8)` and bsdtar reject.
  The parser handles untrusted input (`/set` and `/unset`, continuations,
  vis(3) names) and needs the image parsers' bounds and fuzzing; `contents=`
  stays beneath the source root and link targets are never followed. Write
  the design first. Separately consider an opt-in that applies permission
  bits without setuid, setgid or sticky through opened descriptors,
  directories last, and an explicit trusted-restore profile (owners and full
  modes, as `tar -p` does as root) outside the safe profile; either needs a
  security review of the worker's metadata syscalls.

* [Medium] **Additional inputs and formats** (`lib/source/`, `lib/formats/`):
  design an independent NTFS reader profile as concrete needs arise; QNX4
  is its own initiative above. Follow the
  [extension sequence](docs/implementation-plan.md#extension-sequence); each
  format needs a bounded-resource design and an independent compatibility corpus.

* [Medium] **QNX IFS image support** (`lib/formats/`): read, and later write,
  QNX boot images (`mkifs` output: startup header, image header, flat
  directory, packed and optionally LZO/UCL-compressed file data), such as the
  PCM 3.1 `PCM3_IFS1*.ifs` flash images. Write
  `specs/design-ifs.md` first: header variants and byte orders, compression,
  executable-in-place entries, checksums, and an independent corpus (QNX
  `dumpifs`/`mkifs` output and real images).
