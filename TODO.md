# Open work

## Major initiatives

* [Low] **Rule-derived tables and local policy templates** (deferred; the
  approach needs more design thought before it is scheduled)
  (`tools/relic/`, profile metadata, `lib/formats/`): evaluate adoption of the
  [ordinary-query generation pipeline](specs/design-relic-generation.md) and
  [verification-policy family](specs/design-generated-verification.md) under
  the [generation sequence](docs/implementation-plan.md#scheme-planning-and-relic-generation).
  Use closed result schemas and small AWK renderers with the existing Relic
  CLI. Demonstrate a maintenance benefit across consumers before replacing
  clear native predicates; traversal, grouping, resource ownership and failure
  order stay in C. A generic Relic extension is eligible only on its own merits,
  not a prerequisite of this workflow; no runtime Relic dependency.

* [Medium] **Common driver contract coverage**
  (`test/driver-contracts.c`, `test/layer-contracts.c`, `lib/core/`,
  format/protocol scalar codecs): complete the remaining witnesses under the
  [shared-primitives sequence](docs/implementation-plan.md#driver-contracts-and-shared-verification-primitives):
  [driver contract suites](specs/design-driver-contract-tests.md). On the layer
  side: `compose`, `unwrap` and `open_stream`; layer cancellation, scratch
  limits and scratch-fault sweeps; poisoned layer sinks; encoded-output
  bounds; the output budget with a live raw intermediate; bounded expansion;
  STREAM sinks; and raw-intermediate and spool adoption on refusal. On the
  reader side: memory and scratch limits; the current entry's content
  lifetime; extraction of link and FIFO kinds outside the safe profile
  (refusal and `FSIMG_EXTRACT_SKIP_UNSUPPORTED`); extracted safe metadata;
  two live readers sharing one budget; and an exact per-profile
  `payload_bytes_read`, which is only bounded by the logical bytes.
  A named case for the documented optional-allocation fallback (a refused
  metadata-cache chunk replaced by page replacement): every sweep
  refuses a success after a fired fault, and none of these fixtures reaches
  that fallback.
  The zlib owners' translation of `Z_STREAM_ERROR`, `Z_VERSION_ERROR` and
  `Z_NEED_DICT` also needs deterministic tests. Then retire the common
  scenarios that `test/emit.c` duplicates, keeping its direct-copy and
  corrupt-plan cases. Add scalar-codec boundary evidence for each converted
  record and probe family: truncation at fixed prefixes and field boundaries,
  and golden whole records including QNX6 and ImageFS in both byte orders.

* [High] **QNX6 read/write and automotive qualification**
  (`lib/formats/qnx6/`, `lib/read/`, `lib/write/`): the classic reader,
  verifier and writer of the [Power-Safe design](specs/design-qnx6.md) are
  implemented through its [driver design](specs/design-qnx6-driver.md);
  close the explicit format-evidence gates, and qualify native read/write
  interoperability across automotive and extended variants. Porsche PCM 3.1 HDD reproduction also requires
  metadata fidelity and exact disk composition, including QNX4 where present.
  The [work breakdown](docs/todo-qnx6.md) owns experiments and implementation;
  [research evidence](docs/qnx6-research.md) owns source provenance.

* [Medium] **APFS reader, verifier and writer** (`lib/formats/apfs/`,
  `lib/read/`, `lib/core/`, `lib/codec/`, `cmd/fsimg/`, `tools/apfs/`):
  implement the [format design](specs/design-apfs.md) and
  [driver design](specs/design-apfs-driver.md) in the stages of the
  [APFS plan](docs/apfs-plan.md). Native macOS evidence comes first and
  confirms the reference's tables, the name-hash conventions and the native
  empty-container layout; then the common changes (256-byte label,
  `volume=` and `snapshot=` read settings with `read_settings`, zero and
  held content extents, the 4096-byte key bound, CRC-32C and normalization
  tables, the `case=` write setting); the reader, verifier, compression
  through the imported `lzfse` decoders, and a writer registered only after
  `fsck_apfs -n`, mount and write-after-create pass. Whether socket and
  device entries on native Data volumes need common entry kinds waits for
  the plan's evidence (owner decision 5).

* [High] **Tar-like CLI and image layers** (`cmd/fsimg/`, `lib/layers/`,
  `compat/`): implement the [command-line contract](specs/design-cli.md) and
  the [layer design](specs/design-layers.md) in the four stages of the
  [plan](docs/implementation-plan.md#extension-sequence). Containers and
  gzip are read and written, zstd and xz in a build with their libraries,
  and `-f -` reads and writes streams. The
  `hdiutil attach` gate of the container writer settles the layer design's
  DMG-table-split question.
  Stage 1 leaves these open:
  * The split staging, destination, scratch and root grants and the
    destination fixtures have run only on macOS, which has no sandbox
    backend; run `test-sandbox`, `test-sandbox-setup` and the CLI
    destination tests on Linux (Landlock) and FreeBSD (Capsicum). The raw
    intermediate and spool roles with their Capsicum rights and a stream
    creation's data channel, with its `CAP_FSTAT` right, have likewise run
    only unconfined (`cmd/fsimg/sandbox.c`).
  The layer read side leaves these open:
  The layer write side leaves these open:
  * [Medium] Hole knowledge tests lack the design's Sandbox and capability
    cases: no test spools and transcodes under the worker's installed
    Capsicum rights, none reads an external input limited without
    `CAP_FSYNC` (a discovery sync failing `ENOTCAPABLE`), and none reads an
    input on a filesystem without holes or exercises extraction against
    the discovery-sync counter (`test/sandbox.c`, `test/holes.c`,
    specs/design-holes.md Tests and evidence).
  * [Low] A layer's failure is attributed by `layer_failed` at each of its
    call sites in `lib/layers/image.c` and by the chunk index in
    `lib/layers/chunks.c`; a new layer entry point must remember to call
    it.

* [Medium] **QNX4 interoperability** (`lib/formats/qnx4/`): the
  [format design](specs/design-qnx4.md) and [driver design](specs/design-qnx4-driver.md)
  describe the reader, verifier and writer for native QNX4 volumes and the
  PCM 3.1 navigation partition, and all three are registered. The
  [work breakdown](docs/todo-qnx4.md) holds the open items: the Linux and
  qnxprobe comparisons and head-unit acceptance.
  [Research evidence](docs/qnx4-research.md) owns the native observations.

* [High] **HFS Plus and HFSX** (`lib/formats/hfsplus/`, `lib/codec/`,
  `lib/read/`): the [format design](specs/design-hfsplus.md) and
  [driver design](specs/design-hfsplus-driver.md) describe a reader and
  verifier for native volumes, transparent compression included, and an
  unjournaled writer registered after native macOS qualification; none of it
  is implemented. The [plan](docs/hfsplus-plan.md) orders the stages, from
  the native evidence corpus to wrapped volumes, and the
  [work breakdown](docs/todo-hfsplus.md) holds the open items and decisions.

* [Medium] **Specification-driven testing** (`test/spec/`, `test/spec.sh.in`,
  `Makefile.am`, `.github/workflows/check.yml`): build the Ruby rule, case and
  runner harness. It derives valid and targetedly invalid FAT images from
  cited specification rules, checks fsimg's per-stage status against the
  outcome the rules derive, records independent tools' observations, and
  reports specification coverage. The
  [design](specs/design-spec-testing.md) owns the decisions and the
  [plan](docs/spec-testing-plan.md) the stages.

* [Medium] **Spec harness captures in memory** (`test/spec/lib/fsimg_spec/process.rb`,
  `outcome.rb`, `fat/oracles.rb`): each stage's stdout and stderr are held
  in Ruby strings capped at 64 MiB (`Runner::CAPTURE_BYTES`) and stdout is
  parsed whole, so a case above some 60,000 entries, which the `nightly`
  and `deep` entry limits allow, would fail as `output_exceeded`. The
  oracles' read-back of each file (`mtype` and `fatcat -r`) likewise holds a
  file's bytes whole before hashing them. Stream captures to their
  case-directory files and parse the NDJSON records line by line, give
  `Invocation` a stdout sink that hashes and counts as it reads, and bound
  each case's oracle runs by a total budget.

* [Medium] **Spec harness formatter oracles beyond mkfs.fat**
  (`test/spec/lib/fsimg_spec/fat/bases.rb`): formatter bases are built only
  with mkfs.fat and mtools, so a FreeBSD host without them runs on builder
  bases alone. Add the `newfs_msdos` formatter adapter.

* [Medium] **Spec harness oracles never read targeted or malformed images**
  (`test/spec/lib/fsimg_spec/fat/cases.rb`, `test/spec/lib/fsimg_spec/fat/oracles.rb`):
  the oracles read only valid, unmutated case images, as every profile
  allows unconfined. The nightly and deep profiles should also run them on
  class B and C images through `FSIMG_SPEC_ORACLE_CONFINE`, which the
  preflight checks but no oracle runs through yet, and report those
  observations unavailable without it.

* [Medium] **Spec harness replays no tool-built base**
  (`test/spec/lib/fsimg_spec/fat/cases.rb` `Cases.unreplayable`,
  `test/spec/lib/fsimg_spec/replay.rb`): replay refuses a recipe whose base
  a formatter or mtools builds, so the failure record of an
  mtools-populated case answers 125 under `git bisect run`, although the
  design's replay rebuilds any base and re-classifies one whose tool
  version differs. Rebuild such a base through `Bases.materialize` when the
  host has its tools, and skip it otherwise.

* [Medium] **Spec harness judges a base in two places**
  (`test/spec/lib/fsimg_spec/fat/bases.rb` `Bases.materialize`,
  `fat/cases.rb` `Cases#populated`, `fixtures.rb` `Fixture#problems`):
  `Fixture#problems` and `usable?` are the base verdict the bases tests
  assert, but `Cases#populated`, the one production caller, re-derives its
  own from `geometry_problems` and the observations, with a stricter
  read-back (it must agree, where `Oracles.disagreements` also accepts one
  that does not apply or is unavailable). Make one funnel: `populated`
  acts on the fixture's verdict, keeping its read-back rule there, or the
  tests assert through `Cases#populated` and `problems` goes.

* [Medium] **Spec harness read-back skips drawn creations with supplementary names**
  (`test/spec/lib/fsimg_spec/fat/oracles.rb`, `fat/trees.rb`): mtools
  drops long names beyond the BMP, so the read-back that gives a creation
  case its independent verdict is not applicable to a drawn tree holding
  one, which is most of them (12 of the 15 drawn creations of one quick
  run). Compare the entries mtools can name and report the rest, or draw a
  share of the creation trees from BMP names only.

* [Low] **Spec harness fatcat ignores case bits**
  (`test/spec/lib/fsimg_spec/fat/oracles.rb`): fatcat ignores the case
  bits of a short entry, so a lowercase 8.3 name mtools wrote with them
  reads back in upper case, and fatcat disagrees on an mtools-populated
  case whose tree holds one. Once the model decodes directories (B2),
  report fatcat as not applicable to a name stored with case bits rather
  than as a disagreement.

* [Low] **Spec harness SIGTERM test reads a pid file that may not exist**
  (`test/spec/suite/core/test_invocation.rb` `wait_for_pid`, lines
  182-186): it waits 10 s for the pid file and then reads it anyway, so a
  slow host fails `test_sigterm_to_the_harness_leaves_no_group` with
  ENOENT instead of a timeout message. Seen once on macOS under heavy load.
  Fail with a named timeout, and size the wait to the harness's own bounds.

* [Medium] **FAT integration script into the spec harness**
  (`test/fat-integration.sh.in`, `test/fat.c` `--external` and
  `--make-payload`, `test/fat-write.c` `--create` and `--create-empty`):
  the script is a second interoperability path beside the harness, worth
  moving only as a whole, so that the C hooks go with it. The harness
  still lacks the `newfs_msdos` formatter adapter, a formatter base of
  4096-byte sectors in two-sector clusters, FAT12 fragmentation through
  `mdel`, and an empty created volume of each variant
  (docs/spec-testing-plan.md, B1g).

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
  Drawn trees outside the profile's sample are classified in memory from the
  empty base's boot sector and the drawn tree (`Fat::Cases#defective?`);
  once the model decodes directories, that check must read the decoded tree,
  for example from an image rendered in memory, still without writing it.
  The writer `profile` rule that every byte outside allocated metadata and
  file content is zero needs the decoder's allocation map and lands with it.

* [Low] **Spec harness FAT witness gaps**
  (`test/spec/lib/fsimg_spec/fat/witnesses.rb`):
  - No creation rule states the writer's 2 MiB directory limit, 65536
    slots, which create refuses with `FSIMG_ERANGE`
    (specs/design-fat-writer.md#names-and-directory-layout); the source
    model would expect a wider directory to be accepted. No witness or
    draw comes near it, though a promoted recipe of some 3200 names of 255
    units in one directory would.
  - The recipe loader bounds an exact `-s` by the output ceiling, but not
    the volume automatic sizing gives a loaded creation recipe, such as
    65541 FAT32 clusters of 32 KiB; fsimg would refuse it with
    `FSIMG_ELIMIT` where the rules expect it to be accepted. Bound it by
    `Fat::Sizing.plan` as the witness generator does.
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

* [Low] **One clock, enforced** (`cmd/fsimg/worker-child.c`, `Makefile.am`):
  the worker's progress sampling still calls `clock_gettime` itself (a
  failure only drops a progress record), and no check target confines
  `clock_gettime` to `compat/clock.c` as `check-fstat` confines `fstat`.
  Route it through `fsimg_host_clock` and add the check. The conversion of
  `-l seconds` to a deadline, with its clock-failure and overflow checks,
  is also written twice (`cmd/fsimg/main.c`, `worker-parent.c`
  `prepare_request`); one helper in `compat/clock.c` would hold it.

* [Medium] **A supervision error can cut a record** (`cmd/fsimg/worker-parent.c`
  `supervise_operation`, `cmd/fsimg/main.c`): cancellation, a deadline, or a
  failure of the output relay or of polling ends supervision while the relay
  may be part-way through writing one record, and the relay is then
  stopped, so standard output can end inside that record and the final
  report follows on the same line, leaving `--json` output malformed. A
  worker's failure cannot do this, since control and raw data are read only
  while the relay is idle. Letting the relay finish its one bounded record
  within the exit grace, or starting the final report on a fresh line when
  forwarded bytes went unconfirmed, would keep every earlier record and the
  error record parseable.

* [Medium] **Spec harness stage harness errors** (`test/spec/lib/fsimg_spec/outcome.rb`,
  `case_run.rb`): a stage's `harness_error` outcome covers both host faults
  (fsimg could not be launched, a sandbox status that differs from the
  preflight's) and fsimg's own protocol violations (malformed NDJSON, an exit
  status outside the contract). A case whose expectation such a stage fails is
  `failed`, so a host fault counts as an fsimg failure in coverage and is a
  bad revision to `bin/replay`. Split the host faults out so that they make the
  case a `harness_error`.

* [Low] **Spec harness tests time out under load**
  (`test/spec/suite/core/test_invocation.rb`,
  `test/spec/suite/core/test_runner.rb`):
  `test_sigterm_to_the_harness_leaves_no_group` waits ten seconds for the
  spawned harness to start its stub, then reads the pid file without
  checking it appeared, so under a loaded `make -j32 check` the wait can
  expire and the read raises `ENOENT`. Assert the file's presence with a
  message and give the start a load-independent deadline. The runner tests
  in `test/spec/suite/core/test_runner.rb` share the same exposure: their
  profile's one-second ceiling gives each stub an 11-second wall clock, and
  `test_scratch_file_at_the_ceiling_is_allowed` (a 2 MiB `dd`) has ended
  `timed_out` while other builds loaded the host. Tests that do not test the
  deadline should run under a profile whose wall clock tolerates load.
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

* [Medium] **QNX IFS image support** (`lib/formats/ifs/`, `lib/codec/`,
  `lib/layers/`): write QNX boot images through the `ifsboot` container,
  from a caller's startup program, optional boot prefix and header
  settings, with a project-owned LZO1X encoder. The
  [plan](docs/ifs-plan.md) orders the work; the
  [format](specs/design-ifs.md), [driver](specs/design-ifs-driver.md) and
  [codec](specs/design-codecs.md) designs and the layer design's `ifsboot`
  and `hbcifs` profiles are the contract; the
  [research record](docs/ifs-research.md) holds the evidence; the
  [qualification work](docs/todo-ifs.md) lists the open items.

## Bugs by subsystem

* [Medium] **Post-read source-change payload evidence** (`test/exfat.c`):
  the helper's truncation case changes its source before the next read.
  Add a backing-identity resolver that confirms bytes and then changes the
  backing file, proving the post-read stability failure keeps those bytes
  in an incomplete verification report.

* [Medium] **qemu-img VHD size check fails in an Ubuntu 24.04 container**
  (`test/containers-integration.sh.in`): qemu-img 8.2.2 reports a fixed VHD's
  virtual size from its footer's CHS geometry, failing `w-fat16.vhd: qemu-img
  info size`, while conversion to raw preserves the disk bytes. Accept the
  geometry-derived size or check the converted length instead.

* [Medium] **QNX6 verification decodes short scratch records before checking length**
  (`lib/formats/qnx6/verify.c` `owner_next`, `reference_next`): the stream can
  return a payload shorter than the local record buffer, but both cursors load
  scalar fields before rejecting the length. Check length before any decode or
  output assignment. Add malformed short-record regressions before the fix;
  no external information leak is established, but the reads consume
  uninitialized bytes and violate the scalar span contract.

* [Medium] **Container-plan contract coverage gaps** (`test/ifsboot-write.c`,
  `test/layers-write.c`): the [container-plan design](specs/design-container-plans.md)
  validation list is only partly covered. Add create-path evidence that
  ifsboot prefix reads charge the writer's session, encoder-open, emission
  and cancellation failures each followed by exactly one plan destruction,
  a retained writer plan freed after output failure and after the borrowed
  file settings are gone, and callback sinks proving a bound refusal opens
  and resizes nothing for VHD, DMG and simg.

* [Low] **A failed extraction keeps any error path, not only an entry's**
  (`lib/extract/extract.c` `report_destination`, `include/fsimg.h`): the
  contract keeps a path that names an image entry and otherwise names the
  destination, but the code keeps whatever path is set. It holds because
  nothing on the extraction path sets another; mark an entry path
  explicitly, or record the destination in its own field, so it holds by
  construction.

* [Low] **The job record moves ifsboot settings type-blind**
  (`cmd/fsimg/job.c` `ifsboot_fields`): the table copies `uint32_t` and
  `bool` members with `bcopy` at `offsetof` positions, so a widened member
  would truncate silently past the offset asserts. Encode the six fields
  explicitly.

* [Low] **File settings' Capsicum rights assume no zero-range query**
  (`cmd/fsimg/sandbox.c`, `lib/layers/ifsboot.c`): the input and file
  settings get no `CAP_FSYNC` because nothing asks them for zero ranges, and
  `fsimg_io_next_zero_range` on a host descriptor syncs first. A future
  copy that consults holes on a setting fails `ENOTCAPABLE` on confined
  FreeBSD only. Adopt settings as an object that never consults the host's
  hole report.

* [Low] **test-worker's relay scenario fails setpgid intermittently**
  (`test/worker.c:1103`): under a loaded `make -j32 check` on macOS the
  forked consumer's `setpgid(0, 0)` failed once, while the parent's
  `setpgid(child, child)` at line 1144 races it; five standalone reruns
  passed. Find which errno the child sees and let the child tolerate the
  same race the parent already tolerates (`EACCES`), or move group
  creation to one side.

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

* [Medium] **QNX6 and QNX4 plan rows are trusted when read back**
  (`lib/formats/qnx6/write-plan.c` `qnx6_row_get`,
  `lib/formats/qnx4/write-plan.c` `qnx4_row_get`): both decode every field
  from paged metadata storage, which may spill to untrusted scratch,
  without validating it. Audit where a row field reaches a buffer size,
  copy length or offset, and validate in the one row reader, as
  `ifs_row_get` does for ImageFS.

* [Low] **One classifier for command-line open failures**
  (`cmd/fsimg/paths.c`): `open_input`, `open_operand`, `open_settings` and
  `change_directory_error` each map `ELOOP`, `ENOTDIR` and `ENXIO` to an
  unsupported kind or an I/O failure by hand, and disagree on a regular file
  named with a trailing `/` (unsupported for `-C`, an I/O failure
  elsewhere). The image open also skips `trim_final`, so `image-link/.`
  follows a link to a directory before the kind check refuses it. Route
  every command-line open through `open_nofollow` and one failure
  classifier whose only per-site variation is the noun.

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
  kind/size rule ("link 1..`FSIMG_SYMLINK_TARGET_MAX`, directory and FIFO
  0, file any") one internal owner shared by the catalog and the wire
  validator, so QNX6
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

* [Medium] **The Linux check image repeats the CI job's environment**
  (`build-aux/linux-check/Dockerfile`, `.github/workflows/check.yml`): the
  checks themselves are one list in `build-aux/ci-check.sh`, but the image
  repeats the job's package list and runner tools, and the FreeBSD job's
  setup (ports, fatcat, spec gems) is followed by hand on a host. Run the
  CI Linux job in the same image, and give the FreeBSD setup one script.

* [High] **FreeBSD: QNX4 scale RSS exceeds its bound in the wide case**
  (`test/qnx4.c` scale, FreeBSD 16 on a 128-CPU host): listing or verifying
  one directory of 200,000 entries raises the child's `ru_maxrss` by about
  70 MB against the 32 MiB bound while charged memory peaks at 5.4 MB;
  macOS shows 6-9 MB. It predates the input-sized sort. Ruled out: the
  image mapping (the fixture reads with `pread`), inherited pages (the
  child starts at zero), jemalloc decay (no change; `retain:false` gives
  46 MB; one arena, no change) and the test's region table (about 5 MB).
  Find what holds the rest: uncharged library memory would be a contract
  bug, a FreeBSD accounting effect a test fix. `check-scale` fails on
  FreeBSD until then.

* [Medium] **FreeBSD: `check-cli-rss` creation fails under its address-space
  limit** (`test/qualify-rss.c`, `test/qualify-cli.rb`): FreeBSD bounds each
  process with `RLIMIT_AS` of (budget + 64 MiB) / 3, about 27 MB under the
  16 MiB budget, and `fsimg c` exits unsuccessfully without a diagnostic.
  It predates the zstd and xz libraries and fails without them. A likely
  cause is address space reserved up front (jemalloc's per-CPU arenas on a
  128-CPU host, thread stacks, library mappings) rather than memory used;
  trace the failing call before changing the bound or the method.

* [Medium] **No check runs confinement on glibc 2.33 to 2.38**
  (`build-aux/linux-check.sh`, `.github/workflows/check.yml`): those
  releases implement `fstat()` as `newfstatat`, which the worker's seccomp
  filter denies; `check-fstat` catches a direct call but not a C library
  routine that stats a descriptor internally (`fdopendir`, stdio), and CI's
  glibc 2.39 cannot fail on either. Ubuntu 22.04 (glibc 2.35) built with a
  Landlock ABI 3 `linux/landlock.h`, as stock 22.04 headers compile
  confinement out, failed test-sandbox, test-worker and cli.sh before
  `fsimg_host_fstat` and passes after. Add that environment as a Linux
  check, which would also let `check-fstat` go.

* [Low] **GCC 11 fails the -Werror build** (`lib/source/source.c`
  `open_host_source`, `configure.ac`): Ubuntu 22.04's gcc 11 reports
  `root` and `path` as maybe-uninitialized after `read_host_record`, a false
  positive, and the build stops there, so later files are unchecked. CI and
  the platform checks build with clang. Build with gcc 11 and either
  initialize what it flags or keep -Wmaybe-uninitialized out of -Werror.

* [Low] **Project-owned build-aux helpers need three per-file lists**
  (`.gitignore`, `Makefile.am` `EXTRA_DIST`, `build-aux/check-ascii.sh`):
  autotools installs its own files into `build-aux/`, so each helper is
  re-included, distributed and ASCII-checked by name, and a new helper missing
  from one list goes unnoticed. Move the helpers to a directory autotools never
  writes, so one rule in each place covers every file there.

* [Low] **Static consumers miss the optional libraries' own dependencies**
  (`configure.ac`, `libfsimg.pc.in`): `LZMA_LIBS` (and `ZSTD_LIBS`) come
  from `pkg-config --libs`, not `--static`, and enter `Libs.private`, so a
  fully static consumer of a static liblzma that needs `-pthread` or
  `-lmd` does not get them. Record `Requires.private: liblzma` (and
  `libzstd`) when pkg-config found the library, keeping `Libs.private` for
  a library found on the default paths.

* [Low] **`check-install` does not replay feature options**
  (`build-aux/check-install.sh`): the qualification reconfigures without
  the outer build's `--with-xz`/`--without-xz` or `LZMA_*` (nor `ZLIB_*`),
  so a `--without-xz` tree qualifies an install that found liblzma. Replay
  `config.status --config` as `sanitize.sh` does.

## Security residuals

* [Low] **Shared-library export list** (`Makefile.am`, `lib/`): libfsimg
  exports every external symbol, internal ones included, so a consumer that
  declares an internal function such as `fsimg_writer_new_variant()` itself
  can call it outside the public contract; the compat host layer, such as
  `fsimg_host_private_mkdir()` and the directory-removing
  `fsimg_host_remove_directory()`, is exported too. Export only the public header's symbols
  (a libtool `-export-symbols-regex` or a symbol file, with test programs
  linking the convenience library for internals).

## Planned features

* [Low] **Native evidence for 1024-byte link targets on QNX4 and QNX6**
  (`tools/qnx/`, `docs/qnx4-research.md`, `docs/qnx6-research.md`): the
  QNX4 and QNX6 limits of 1023 bytes are the bound fsimg holds evidence
  for, not a format limit. Create a 1024-byte target on each filesystem in
  the QNX 6.5.0 guest and record whether the filesystem stores and reads
  it back; a positive result raises `max_link_target_bytes` to 1024 in the
  format designs and descriptors.

* [Medium] **End-to-end CLI tests of omitted names** (`test/cli/run.rb`,
  `test/spec/`): the omission contract is tested in the library
  (`test_omitted_names` in `test/qnx4.c` and `test/qnx6.c`) and in the CLI
  display and worker parent with fabricated results, but no test runs the
  real worker on a QNX image holding an out-of-profile name. fsimg's
  writers refuse such names, so the image needs a synthetic fixture or a
  spec-harness generator; then check `t`, `W` and `x -k` print the warning
  and the JSON `omitted_entries`, `x` without `-k` fails naming the
  rendered path, and `t -v` shows no omitted entry. The same fixture covers
  the worker's LIST and VERIFY jobs carrying a nonzero omitted count, and
  the parent's refusal of one from a CAT job (`test/worker.c` checks only a
  forged extraction count today), and a namespace collision, whose
  scratch-record path check has no test seam.

* [Medium] **More than two FATs** (`lib/formats/fat/`, `cmd/fsimg/options.c`,
  `specs/design-fat-reader.md`, `specs/design-fat-writer.md`,
  `specs/design-sizing.md`): the Microsoft specification allows any nonzero
  `BPB_NumFATs`. The reader supports one or two and reports a larger count as
  `FSIMG_EUNSUPPORTED` (`FAT_READ_TABLES` also sizes the table window); the
  writer always emits two. Design per-copy mirror checks within the bounded
  window, FAT32's four-bit active-table field, a writer setting, sizing and
  the capability record; then implement both sides and check mtools and
  `fsck.fat` behaviour with more than two tables.

* [Medium] **Manifest import and export** (`lib/source/`, `lib/extract/`,
  `lib/core/`, `cmd/fsimg/`, `fuzz/`): implement the
  [manifest design](specs/design-mtree.md). The parser, the source root
  (`fsimg_writer_add_manifest()`, `lib/source/manifest.c`,
  `lib/source/generated.c`, `test/manifest.c`) and the `c` mode's `@file`
  operand (`cmd/fsimg/paths.c`, `cmd/fsimg/worker-child.c`, `test/cli/run.rb`)
  and `x --mtree=FILE` (`lib/extract/mtree-writer.c`) exist. What remains
  is reading the manifest `x --mtree` writes with `mtree(8)` on FreeBSD and
  Linux (`test/manifest.c` checks it with bsdtar only); the refusal of an
  empty, overlong or NUL-holding link target (`record_skipped`), which
  needs a crafted image since the writers refuse such targets; FILE
  appearing between the early check and publication; the worker's failed
  close of the manifest, which no mechanism here can make fail on a regular
  file; and a staged image or
  manifest retained by a publication that is unsupported or uncertain,
  which needs a filesystem refusing hard links or a failing directory sync
  (`test/cli.c` checks only how a retained path is reported). With `-E`, a
  FILE inside the destination is refused as a non-empty destination, since
  the manifest's staging directory is made first; the targeted overlap
  message is unreachable there. An exFAT round
  trip through `x --mtree` is not byte-equal for a nonempty file of
  generated zero content: planning (`lib/formats/exfat/write-plan.c`) gives
  it an initialized length of zero but gives a host file, which its
  extracted copy is, its full length whatever holes the host reports, so
  the rebuilt image initializes it (`test_export_exfat_zero_file` in
  `test/manifest.c`). Planning a host file that is one hole as zero storage
  would close it.

* [Medium] **Extended attributes in the common entry model** (`include/`,
  `lib/source/`, `lib/read/`, `lib/extract/`, `lib/formats/apfs/`): APFS
  stores Finder information, resource forks, quarantine flags and other
  metadata as extended attributes, which the entry model cannot carry, so
  the APFS reader exposes none and its writer writes none, as the
  [APFS design](specs/design-apfs.md#extended-attributes) states. Design a
  bounded common representation (names, sizes, content delivery, loss
  policy, safe-extraction stance) before any format advertises it; the
  hardlink-topology item in [the QNX6 TODO](docs/todo-qnx6.md) is the
  analogous gap for links.

* [Low] **Opt-in metadata restore** (`lib/extract/`): consider an opt-in that
  applies permission bits without setuid, setgid or sticky through opened
  descriptors, directories last, and an explicit trusted-restore profile
  (owners and full modes, as `tar -p` does as root) outside the safe profile;
  either needs a security review of the worker's metadata syscalls.

* [Medium] **Additional inputs and formats** (`lib/source/`, `lib/formats/`):
  design an independent NTFS reader profile as concrete needs arise; QNX4
  is its own initiative above. Follow the
  [extension sequence](docs/implementation-plan.md#extension-sequence); each
  format needs a bounded-resource design and an independent compatibility corpus.
