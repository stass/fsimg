# Specification-driven testing plan

This plan defines and orders the work for specification-driven testing of
fsimg. It is a Ruby test layer built on Minitest, PropCheck and BinData:

* Named rules, cited to the published format specification, describe valid
  and targetedly invalid filesystem images.
* The harness builds those images, runs fsimg on them stage by stage, and
  checks the results against the rules and against independent tools.
* It reports **specification coverage**: for each rule, its valid and invalid
  witnesses, boundaries, the stages exercised, the oracle classifications and
  stable reproduction recipes.

FAT12, FAT16 and FAT32 come first, then exFAT. QNX6 and later formats follow.

The working agreement applies unchanged. The design document comes before
code. Each stage gets the review subagent and the OpenCode GLM 5.3 max review,
and a failing regression test comes before any fix. Each stage ends in one
coherent commit, and skipped checks are reported. No calendar estimate
substitutes for a gate.

## Goals and non-goals

Goals:

1. Changing one field domain or rule expands or changes the generated cases
   without edits to individual tests.
2. The harness generates **valid** cases and **targetedly invalid** cases, each
   with an expected fsimg outcome derived from the rules.
3. It tests reading through `info`, `ls`, `extract` and `verify`, and it
   tests creation independently, with external readers.
4. It catches:
   - arithmetic errors and mishandled malformed metadata;
   - endless loops and resource exhaustion;
   - unsafe extraction paths;
   - incompatibilities with other implementations.
5. Failures are small, reproducible recipes that can be promoted to permanent
   regression cases.
6. The core (rules, cases, runner, reports) serves every format without forcing
   formats into one schema.
7. The quick profile runs inside `make check`. Nightly, deep and fuzzing runs
   are separate, bounded jobs.

Non-goals:

* A formal proof of compliance, or exhaustive enumeration of every field
  combination or filesystem graph.
* A new expression language, a YAML or JSON rule DSL, `eval`, or a compiled
  binary-format DSL. Rules are plain Ruby procs, and only their metadata and
  inputs are serialized.
* A full FAT writer, a filesystem driver or a virtual block device in the
  harness. The independent builder is deliberately small; see
  [fixtures](../specs/design-spec-testing.md#fixtures).
* A solver dependency in development or CI; see
  [generation and shrinking](../specs/design-spec-testing.md#generation-and-shrinking).
* Declaring every noncanonical, recoverable or advisory field mismatch
  invalid.
* Treating one reference implementation or checker as the specification.
* Mounting any generated image through the host kernel.

## What already exists

The harness adds to the current suites. It does not replace them.

| Existing asset | Where | Relevance |
| --- | --- | --- |
| White-box FAT corruption tests | `test/fat.c`: `test_corruption` (16 scenarios), `test_detection`, `test_geometry`, `test_thresholds`, `test_cluster_ceiling`, `test_mirror_selector`, `test_long_names` | They build images in C with `fixture_geometry`, patch them with `put16`/`write_bytes`, and use fault injection. They stay. The Ruby layer is a black-box, independently authored check. |
| Writer round trips | `test/fat-write.c` (`test_roundtrip`, `test_layout_boundaries`, `create_external`) | Writer-side rules in milestone B complement them. |
| External-oracle integration | `test/fat-integration.sh.in` | Builds a fixed matrix with mkfs.fat or newfs_msdos and mtools, then checks with fsck and fatcat. Its oracle detection and skip behaviour are the model for the Ruby adapters. |
| Comparison baseline | `test/baseline.sh.in`, `test/baseline.txt` | Out of scope. The harness never changes the baseline fixtures. |
| libFuzzer targets | `fuzz/fat.c`, `build-aux/fuzz.sh` | Seeds are generated at run time into `fuzz-corpus/` with a 3 MiB maximum length. No corpus is committed. |
| Interpreter precedent | `test/cli.sh.in` with `test/cli.py` | Python is an optional development tool. Its wrapper exits 77 when the interpreter is absent. The Ruby wrapper follows this pattern. |
| CLI stages | `i`, `t`, `x -O`, `x -E`, `W [-d]`, `c` (`specs/design-cli.md`) | The runner drives these. With `--json`, they emit NDJSON `summary` and `error` records. Each error record carries the integer status `code` (an `fsimg_status_t` value) and a prose `status` string. |

## Design

[specs/design-spec-testing.md](../specs/design-spec-testing.md) owns the
target design and the decisions: placement and toolchain, the three kinds of
specification, rules, policies and expectation derivation, case classes and
recipe schema 1, mutations, the runner contract and outcome classes, fixtures
and independence, generation and shrinking, profiles, reports and failure
records. This plan orders the work and records what each stage must
demonstrate.

## Build, dependencies and CI

* **configure.ac:** `RUBY` and `MDIR` as optional development tools, and
  `test/spec.sh` in `AC_CONFIG_FILES`.
* **`test/spec.sh.in`:**
  - It exits 77 with a `SKIP:` message when Ruby is missing or older than
    3.2, and sets `BUNDLE_GEMFILE`.
  - `run.rb` loads the bundle through Bundler with the frozen and
    no-auto-install settings, which outrank any Bundler configuration, so a
    test run never rewrites the lock, installs a gem or reaches the network.
    It exits 77 with a `SKIP:` message naming
    `bundle install --gemfile=test/spec/Gemfile` when Bundler or a locked gem
    is absent, and fails on any other load error. Loading through the
    interpreter that runs the tests, rather than a separately found `bundle`,
    checks that interpreter's gems.
  - It exports `FSIMG_BIN`, the build directory as `FSIMG_BUILDDIR`, and the
    tool paths that `configure` found: `MKFS_FAT`, `FSCK_FAT`, `NEWFS_MSDOS`,
    `FSCK_MSDOSFS`, `MCOPY`, `MMD`, `MDEL`, `MDIR` and `FATCAT`.
  - It takes the profile as its argument, quick when none is given, and runs
    `run.rb --profile=PROFILE`. `make check` runs it without one, so no
    exported variable changes the profile of `make check`.
* **Makefile.am:**
  - `test/spec.sh` joins `TESTS`.
  - New targets: `check-spec` (quick), `check-spec-extended` (nightly) and
    `check-spec-deep` (opt-in).
  - `check-extended` gains `check-spec-extended`.
  - `EXTRA_DIST` names the tracked roots of `test/spec/` (`Gemfile`,
    `Gemfile.lock`, `run.rb`, `lib`, `suite`), not the directory, so a local
    `.bundle` or `vendor` never ships. The stages that add `bin` and `cases`
    add them here and to the header self-test's glob.
* **ASCII gate:** `build-aux/check-ascii.sh` also scans `*.rb`, `*.json`,
  `Gemfile` and `Gemfile.lock`. Every Ruby file starts with the SPDX
  BSD-2-Clause header and `# frozen_string_literal: true`.
* **Licences:** minitest (MIT, with its dependencies drb, BSD-2-Clause, and
  prism, MIT), prop_check (MIT, no dependencies) and bindata (BSD-2-Clause),
  used as the
  [placement section](../specs/design-spec-testing.md#placement-and-toolchain)
  states; the GPL oracles are used as
  [independence](../specs/design-spec-testing.md#independence-and-oracles)
  requires. `docs/references.md` records the oracles.
* **CI** (`.github/workflows/check.yml`):
  - macOS: `brew install ruby`.
  - Ubuntu: `apt-get install ruby ruby-dev ruby-bundler`, giving Ruby 3.2;
    `ruby-dev` lets the install compile Minitest's prism dependency.
  - FreeBSD: the `devel/rubygem-bundler` port, which brings the default Ruby
    port; configure is given the interpreter `bundle` runs on, which may be
    named `ruby3x`.
  - Every job runs `bundle install` into a path outside the checkout. The
    GitHub-hosted jobs cache it by host, Ruby version and lock; the FreeBSD
    VM installs afresh.
  - The GitHub-hosted jobs upload only each profile's `coverage.json`, text
    summary and `failures/` with the logs. The GCC container jobs install no
    Ruby and skip the suite.
* **docs/build.md** gains the setup, the profiles, the environment variables,
  the replay command and the report location. It also states the explicit
  skips per host, as it already does for the other oracles.
* Ruby style follows the [Ruby section of CLAUDE.md](../CLAUDE.md#ruby).
  The ASCII gate and the review checklist apply it to `test/spec/`.

### Profile budgets

S6 and S7 measure the
[profile](../specs/design-spec-testing.md#profiles) budgets.

## Work packages

Milestone A is S0-S8. Every stage leaves `make check` passing and follows the
review and commit gates.

### S0. Design document and indices

* Write [`specs/design-spec-testing.md`](../specs/design-spec-testing.md), the
  target design in the present tense.
* Reference it from `specs/README.md` and from a short testing section in
  `specs/architecture.md`. That section records only the cross-subsystem
  contract: the integer status `code` of CLI `--json` error records, what
  `FSIMG_EFORMAT`, `FSIMG_ECORRUPT` and `FSIMG_EUNSUPPORTED` mean, including
  the host causes of `FSIMG_EUNSUPPORTED`, and the `FSIMG_EIO` and
  `FSIMG_ESTATE` fault classes, which image content never causes on a
  working host.
* **Gate:** the design is reviewed and the owner accepts it.

### S1. Toolchain skeleton

* Add `Gemfile` and `Gemfile.lock`, `run.rb`, `test/spec.sh.in`, the
  configure and Makefile wiring, the ASCII-gate extension, the CI steps, and
  the build.md and style.md sections.
* Lock the gem versions after confirming that PropCheck shrinking works
  inside Minitest assertions on both Ruby 3.2 and 4.0.
* One self-test proves the wrapper, the profile selection and report writing.
* **Gate:**
  - `make check` passes with Ruby and gems present, and skips with exit 77
    when Ruby is absent and when the gems are absent;
  - `make distcheck` passes;
  - `check-ascii` rejects a planted non-ASCII `.rb` file;
  - all three CI hosts run the suite rather than skipping it.

### S2. Core model

* `Rule`, `Domain`, `Expectation`, `Case` and the byte-level
  [mutations](../specs/design-spec-testing.md#mutations): `set_u8`, `set_u16le`,
  `set_u32le`, `replace_bytes` and `truncate`. `image.rb` applies them
  sparsely.
* Self-tests, each paired with a seeded failure that proves the check ran:
  - Mutations apply at their bounds; an offset or value outside the target
    and a truncation that would lengthen the image are refused.
  - Recipes round-trip to identical JSON, and the loader refuses one
    recipe for each refusal the
    [recipe design](../specs/design-spec-testing.md#recipes) lists.
  - Derivation over synthetic rule sets covers single and mixed violations,
    fixed and per-violation scopes, status sets, accepting stages under
    `safety_only` whose records must still match the model, and unclassified
    scopes. Moving one rule's scope changes exactly the stages
    that check it.
  - A sparse copy keeps its holes on APFS and ext4, and a dense copy fails
    the same hole check. A host that cannot verify holes reports a skip.
* **Gate:** the self-tests pass. A change to a test domain changes the
  derived expectations without any per-case edit.

### S3. FAT layout, model and geometry

* `layout.rb` holds BinData records for:
  - the common BPB (offsets 0-35);
  - the FAT12/16 extension (36-61) and the FAT32 extension (36-89), which are
    mutually exclusive;
  - the signature at 510;
  - FSInfo;
  - the 32-byte directory entry.

  A FAT32 extension is never read over a FAT12/16 BPB.
* An offset table, transcribed by hand from the specification, is asserted
  against every field. It is independent of the C sources.
* `model.rb` decodes any byte prefix into a model with explicit `Undefined`
  markers.
* `geometry.rb` derives these quantities without raising on zero divisors,
  overflow or contradictory fields:
  - root-directory sectors and the first data sector;
  - the data-cluster count, the variant and the maximum valid cluster;
  - the required FAT bytes, which take the 12-bit packing into account.
* Tests cover:
  - the cluster-count thresholds;
  - FAT capacity at the exact fit and one entry short;
  - the 16/32-bit total and FAT-size field selection;
  - random field tuples (PropCheck) that must never make the model raise.

  Seeded failures: two swapped offsets in the transcribed table fail the
  layout test, and a geometry function that divides by the sector size
  fails on a zero sector size.
* **Gate:** the layout offsets are reviewed against the specification text.
  No PropCheck run makes the geometry code raise.

### S4. Runner

* Implement the [runner contract](../specs/design-spec-testing.md#runner),
  the preflight, and `tree.rb` with the name funnel.
* Give `Expectation` a structured form, the allowed statuses plus
  any-verdict and unclassified markers, with one method that decides whether
  an observed outcome satisfies it, so the runner never re-parses outcome
  strings.
* Cover the accepting stages under `safety_only`, whose records must still
  match the model.
* Self-tests use stub executables written into the test's temporary
  directory. Each stub demonstrates one failure:
  - it never exits;
  - it forks a child that never exits;
  - it floods stdout;
  - it dies by `SIGSEGV`;
  - it prints a fake sanitizer report;
  - it exits 1 with an `FSIMG_EIO` error record;
  - it exits 1 with no error record;
  - it emits malformed NDJSON;
  - it writes a symbolic link and a file outside the destination;
  - it changes the image;
  - it leaves a child alive, which the group probe after the reap must
    report;
  - it writes a scratch file as large as the profile's scratch ceiling,
    which `RLIMIT_FSIZE` must allow without `SIGXFSZ`;
  - it exits with status 87, which must classify as `sanitizer_failure`,
    and it reports all four sanitizer option variables ending in
    `abort_on_error=0:log_path=stderr:exitcode=87`;
  - it writes `.mtoolsrc` into its case directory, which must not reach the
    next oracle; and a stub whose safety check fails must end its case
    before any oracle runs.

  Each must be classified correctly, and no process of the group may
  survive. Two cases with one id in a run must give `harness_error`.
* The preflight fails the run when the longest walked root (`dest/` or
  `source/` with a 128-byte id, or `test/spec-cache/<key>.<hex>.tmp.src/`)
  plus the profile's `path` ceiling exceeds the host's `PATH_MAX`, when the soft
  `RLIMIT_NOFILE` is below the `fds` limit, when `trials/` and `cases/`
  differ in `st_dev`, and when the oracle confinement command lets a write
  outside the case directory or a read of the sentinel succeed (for example,
  `env`).
* The name funnel refuses an empty name, `.`, `..`, `a/b` and a name with
  NUL without creating anything, and creates the exact bytes of a name whose
  Ruby string is tagged ISO-8859-1 or CP437. On default APFS a case pair and
  an NFC/NFD pair answer `EEXIST`, and a component of 255 U+00E9 succeeds; on
  ext4 the pairs succeed and that component answers `ENAMETOOLONG`. Seeded
  failures: a funnel that treats `EEXIST` as success makes a case-pair
  witness fail at `extract` on APFS, and one that builds paths with
  `File.join` creates `C3 A9` for an ISO-8859-1 `E9`. A creation recipe with
  a case pair is a host skip on APFS and runs no fsimg. A promoted case
  whose tree holds an NFC/NFD pair replays on both APFS and ext4.
* A depth witness at the profile's depth ceiling extracts without
  `FSIMG_ELIMIT:fds`.
* Real fsimg invocations check the fault side of the
  [outcome classes](../specs/design-spec-testing.md#outcome-classes): one
  in which the harness kills the parser worker is classified `faulted`,
  never `rejected`, and a `-S required` run on a host without confinement
  fails the preflight as `harness_error`.
* Items that wait for later stages:
  - the `.mtoolsrc` stub needs the oracle runner of milestone B;
  - the promoted NFC/NFD replay needs promoted creation cases with name
    trials;
* **Gate:** every stub is classified as above, with no leftover processes
  (checked by process group), and the runner drives the real `fsimg` through
  every stage on a trivially valid image.

### S5. Base fixtures and the independent builder

* Build the formatter adapters and capability flags for:
  - `mkfs.fat` and `fsck.fat` on macOS and Linux;
  - `newfs_msdos` and `fsck_msdosfs` on FreeBSD;
  - mtools for population and listing;
  - fatcat, as a secondary reader where `configure` found it.
* An absent oracle is reported as unavailable for that oracle only. Core
  tests still run on bases from the builder.
* Add the cache and the manifests. A formatter base's population tree is
  written by the name funnel into a directory of its build's own,
  `test/spec-cache/<key>.<hex>.tmp.src/`, and removed once the base is built.
* Add the independent empty-volume builder.
* Give the preflight its sample image from a builder base instead of its
  own `create` output, so the preflight does not depend on the writer it
  is about to test.
* Classify a creation case per stage: `create` follows the source tree's
  classification, and the reading stages that run on the created image
  follow the image's. A rejecting `create` leaves those stages nothing to
  run. `Case#recorded` and `Case#drift` refuse a creation case until the
  case carries both classifications and derives each stage's recorded
  expectation from the right one.
* Base set:

  | Base | Size | Sector bytes | Sectors per cluster | Tree |
  | --- | --- | --- | --- | --- |
  | FAT12 | 1440 KiB | 512 | 1 | none |
  | FAT16 | 16 MiB | 512 | 1 | none |
  | FAT32 | 64 MiB | 512 | 1 | populated |

  - The FAT32 tree holds `HELLO.TXT`, a long Unicode name, a nested
    directory and a multi-cluster file.
  - The builder adds bases at each threshold, with one, two and three
    FATs, and for 4096-byte sectors.
* Measure the materialization and copy costs, and record them in build.md.
* On Linux, confirm that mtools under the oracles' UTF-8 locale keeps a
  Unicode long name that `LC_ALL=C` would lose.
* Seeded failure: a builder base rendered with one field off its parameters
  is refused at materialization.
* **Gate:**
  - every base passes the read-only checker and `mdir`, or a reason is given
    per host;
  - a builder base and a formatter base with the same geometry decode to
    equal models;
  - fsimg `info`, `ls`, `extract` and `verify` accept every supported base,
    and the extracted tree matches the recipe.

### S6. FAT rule registry and generators

* Implement the [initial rule set](#initial-fat-rule-set) with citations of
  the specification's field names. Exact section numbers are taken from the
  PDF, not from memory.
* For each rule:
  - deterministic cases: the legal list, the invalid examples, and n-1, n,
    n+1 at every boundary;
  - PropCheck draws over the field's full width.
* Mark each case `single_rule` or `multi_rule`, and report collateral
  violations.
* Run cases end to end within the quick budget: valid witnesses and those of
  `must_accept` rules through every reading stage, and the witnesses of any
  other policy through the first stage that checks their rule.
* Inventory the existing `test/fat.c` corruption and detection cases against
  the rules. Every gap either becomes a Ruby case or is noted in the design
  as intentionally white-box.
* Validate the interfaces against injected faults before expanding. For
  example, temporarily mis-set a rule's scope or status, or patch a local fsimg
  build to skip one check, and confirm that the harness reports each fault
  precisely.
* **Gate:**
  - changing a rule's allowed values changes the generated cases without
    editing per-case code;
  - at least one targeted invalid case fails its intended rule, and the
    report names any collateral violations;
  - fsimg behaviour, not only Ruby predicates, is asserted for
    representative valid and invalid cases;
  - a saved recipe replays against a fixed base with identical relevant
    bytes.

  Every mismatch is triaged as an fsimg bug (regression first), a rule bug,
  a missing rule, or a policy question for the design document.

### S7. Reports, failures and regressions

* Implement the coverage report, failure records, `bin/replay`, `bin/report`
  and the promotion procedure of
  [reports and regressions](../specs/design-spec-testing.md#reports-and-regressions).
  `spec.sh` prints the text summary at the end of a run.
* **Gate:**
  - the report shows the real counts for the S6 run;
  - an injected fault produces a failure record whose `replay` reproduces it;
  - at least one minimized negative recipe is promoted under a
    `regression.` id, and editing its recorded expectation fails the drift
    check.

### S8. Fuzz seeds and sanitizer pass

* `bin/export-seeds DIR` writes the deterministic invalid witnesses and the
  promoted cases as fuzz seeds, as the
  [design](../specs/design-spec-testing.md#profiles) states: an image within
  the 3 MiB maximum length whole, a larger one without its zero tail.
* `build-aux/fuzz.sh` adds these seeds to `fuzz-corpus/` when Ruby 3.2
  and the locked gems are available, and is otherwise unchanged. The
  native harness never calls Ruby inside a fuzzing iteration.
* Run `check-asan`, `check-ubsan`, `check-lsan` and `check-msan` on the
  final milestone tree with the spec suite inside. Record per host in
  build.md which sanitizer checks ran and which were skipped.
* **Gate (milestone A done):**
  - the quick suite runs with documented setup on macOS and Linux, and
    FreeBSD results or skips are recorded;
  - the coverage report holds real outcomes;
  - every included BPB and geometry rule has deterministic valid and invalid
    boundary cases;
  - representative extraction and creation output is checked independently;
  - at least one minimized, reproducible negative recipe exists;
  - no solver is needed;
  - the sanitizer suites pass with the spec suite included, and any report
    is fixed at its root cause;
  - `check-fuzz` runs with the exported seeds.

### Milestone B: semantics and writer/reader oracles

* **B1, tree model and cross-oracles**, as the reviewed commits B1a to B1g.
  Each leaves `make check` passing and follows the review and commit gates.
  Until B2's decoder, the model classifies the `directories`, `files` and
  `verification` scopes with a **known tree**: an unmutated base carries its
  recipe's tree as its decoded tree, whether the independent builder rendered
  it or mtools populated it, and an image fsimg created carries the source
  tree the harness walked back. A mutated image leaves those scopes
  unclassified. Shrinking of drawn trees stays in B4.
  - **B1a, core record checks.**
    - `Model#record_problems` compares per stage: `ls` paths, types and
      sizes; `extract` the walked `dest/` tree with SHA-256 digests; `verify`
      the file, directory and logical-byte counts, each against the known
      tree.
    - A creation `Case` carries both classifications, the source model's for
      `create` and the created image's for the reading stages, so
      `Case#recorded` and `Case#drift` accept it; creation recipes record and
      replay; the case phase supplies the `created` hook. A created image
      must classify with no violation, or `create` fails, as rule 3 of the
      [expectation derivation](../specs/design-spec-testing.md#expectation-derivation)
      states: the image is fsimg's output, not a generator's.
    - Seeded failures: a stub `ls` that drops one entry, an `extract` whose
      file differs by one byte, and a `verify` that reports one file too few
      each fail their case; a creation recipe with an edited recorded
      expectation fails the drift check.
    - **Gate:** every valid and `must_accept` witness on an unmutated base
      asserts acceptance at `ls`, `extract` and `verify` with records that
      match the known tree; a creation recipe round-trips through a failure
      record and `bin/replay`.
  - **B1b, name trials and destination rules**, as the design's
    [host names](../specs/design-spec-testing.md#host-names) section states.
    - Each extract case's sibling sets go through `trials/`; `EEXIST` gives a
      `destination` violation with `FSIMG_EEXISTS`, `ENAMETOOLONG` and
      `EILSEQ` one with `FSIMG_EUNSUPPORTED`; answers are cached per run by
      the set's bytes; recipes never record destination violations.
    - Seeded failure: a funnel that treats `EEXIST` as success makes a
      case-pair witness fail at `extract` on APFS.
    - **Gate:** `extract` is classified; the promoted NFC/NFD replay that
      S4 left waiting passes on APFS and ext4.
  - **B1c, tree generator.**
    - PropCheck generators draw the small trees the
      [design](../specs/design-spec-testing.md#generation-and-shrinking)
      lists, with its definition of a reserved name and its character
      restriction for case-equivalent siblings.
    - The profile gains the number of drawn trees and the sample that runs
      fsimg; the case phase draws the sample from a generator seeded per run
      and records `random_seed` in each sampled recipe; builder tree cases
      join the run.
    - Seeded failure: a generator that emits a trailing-space name classifies
      its tree invalid and is reported as a generator defect.
    - **Gate:** a quick run reports drawn tree cases run through fsimg, each
      replayable from its recipe without the seed.
  - **B1d, oracle layer.**
    - Population is separated from formatting, so mtools populates any empty
      base, builder or formatter.
    - The case phase gets the tool set, the oracle shell and `oracle-home/`;
      the observation type (`agrees`, `disagrees` with detail,
      `not_applicable`, `unavailable`) is recorded per tool in the case
      result, the failure record and the coverage report.
    - Seeded failure: a planted `.mtoolsrc` in a case directory does not
      reach the next oracle, which closes the S4 stub that waited for this
      layer.
    - **Gate:** an mtools-populated builder base decodes to its recipe's
      tree and passes the checkers, or the report says per host why not.
  - **B1e, external create, fsimg read.**
    - mtools-populated cases run in the case phase; each drawn tree is
      populated into a copy of one cached empty base, never a formatter run
      per draw.
    - **Gate:** fsimg `x` of every populated case equals the known
      tree in path, type, size and SHA-256, and the quick budget holds.
  - **B1f, creation cases.**
    - A source model evaluates the `input` rules: setting domains, name
      validity, case-equivalent siblings and file size.
    - After an accepting `create`, `mcopy -s` reads `out/created.img` with the
      verdict the design gives it, `fsck -n` and fatcat are recorded, and a
      second `create` from the tree written in reverse order into
      `source-reversed/` writes `out/reversed.img`, which must equal
      `created.img` byte for byte.
    - Seeded failures: a source tree with a trailing-dot name must classify
      as an `input` violation and `create` must reject it; a stub `create`
      that writes different bytes for the reversed source fails the case.
    - **Gate:** creation cases of drawn trees pass through every stage;
      disagreements are recorded per tool.
  - **B1g, writer geometry witnesses.**
    - Automatic sizing margins: cluster counts 16 from each threshold
      (FAT12 at most 4069, FAT16 4101 to 65509, FAT32 at least 65541).
    - Exact `-s` sizes giving 4084, 4085, 65524 and 65525 clusters, and
      `-o sector=` with `-o cluster=` combinations, including 4096-byte
      sectors.
    - Then the decision whether `test/fat-integration.sh` moves into the
      harness. It is worthwhile only if it removes a mechanism, which needs
      the `newfs_msdos` adapter, FAT12 fragmentation through `mdel`, and
      CLI-level writer checks first; the script, `test-fat --external` and
      `--make-payload`, and `test-fat-write --create` and `--create-empty` go
      with it.
    - **Gate:** each threshold witness classifies as intended, fsimg creates
      and reads it, and the oracles' observations at the thresholds are in
      the report.
* **B2, semantic mutations.**
  - `model.rb` gains a bounded FAT-table and directory decoder for small
    images. It replaces the known tree: an unmutated base and a created image
    must decode to the tree B1 assigns them, and a mutated image classifies
    its directory scopes.
  - The writer `profile` rule that every byte outside allocated metadata and
    file content is zero, which needs the decoder's allocation map.
  - With the decoder, `Case#harness_defect` again refuses a valid case that
    leaves a scope unclassified, as the cases design requires.
  - The mutations are the semantic
    [mutations](../specs/design-spec-testing.md#mutations) (`set_fat_entry`,
    `set_directory_entry`, `splice_chain` and `relocate_chain`), plus LFN
    order, checksum and UTF-16 faults, FSInfo signatures and hints, backup
    boot divergence, and mirror divergence.
  - The rules include the `test/fat.c` cases that S6 left without one:
    FAT chains (a cycle, a link to a free, bad or out-of-range cluster, a
    short or long chain, a directory cycle, crosslinks, an orphan, a dirty
    `FAT[1]`); directory entries (a duplicate short name, timestamps,
    attribute `0x80`, a shared or ancestor directory, `.` and `..`, labels);
    names and long names (a lowercase short name, `/`, a leading space, LFN
    ordinal, checksum, padding, type, orphan and collision faults); FSInfo
    contents; backup boot contents; and mirror agreement. With them come
    `fat.fat.media_entry` and `fat.fat.eoc_entry`.
  - Expected stages follow the
    [stage policy](../specs/design-spec-testing.md#stage-policy), including
    its file-chain crosslink example.
* **B3, metamorphic checks.**
  - Relocating a chain leaves the extracted tree unchanged and `verify`
    accepting.
  - Setting the reserved high four bits of FAT32 entries, under the
    `equivalent_contents` rule `fat32.fat.entry_reserved_bits`, leaves every
    stage accepting with the same records; the reader masks them
    (`lib/formats/fat/read.c:102`), so `verify` accepts too.
  - Setting the FSInfo hint to unknown (`0xFFFFFFFF`) leaves every stage
    accepting.
  - Directory-slot reordering and physical creation order do not change the
    extracted tree.
  - Changing boot code or a permitted label changes nothing beyond what the
    rules allow.
  - Every transformation is itself checked against the rules before it is
    used.
* **B4, recipe shrinkers,** as described under
  [generation and shrinking](../specs/design-spec-testing.md#generation-and-shrinking).
* **B5, finder attribution.** Each defect found is attributed to a source:
  deterministic boundaries, random examples, semantic mutations,
  compatibility checks or libFuzzer. Coverage and defect classification are
  kept separate.
* **Gate:**
  - an injected cluster-chain cycle, a crosslink and an undersized FAT are
    detected and reproduced, with no sanitizer report, hang or false `valid`
    classification;
  - a fragmented file and an LFN case extract correctly.

### Milestone C: native fuzzing changes from evidence

* Measure Ruby invocation cost, materialization cost, and the native coverage
  that recipe seeds add compared with plain byte mutation, under equal CPU
  budgets.
* Only if the measurement shows a gain, add a libFuzzer target over a compact
  patch stream. The stream names a base, an operation and a bounded list of
  patches (`set_bpb`, `set_fat_entry`, `change_file_size`, `truncate`),
  applied through a copy-on-write sector overlay. Ruby generates its initial
  corpus and decodes minimized failures.
* Promote crash and sanitizer findings as deduplicated regression recipes.
* fsimg gains no virtual-disk abstraction for testing alone.
* **Gate:** a coverage gain or a new regression under the same CPU budget as
  byte-level fuzzing. Without one, record the measurement and ship no second
  harness.

### Milestone D: exFAT

* Port the harness to exFAT: layouts, predicates, recipe constructors and the
  exfatprogs adapter, reusing the rule, case, runner and report core. Extract
  new shared abstractions only where the port exposes real duplication.
  Richer graph models for later formats are not built ahead of the port
  that needs them.
* QNX6 follows. Its valid images come mainly from the builder, and the
  QNX 6.5.0 VM harness (`docs/qnx6-infrastructure.md`) serves as a manual
  oracle. The harness can then serve the QNX6 writer's acceptance evidence.
* **Gate:** the core runner, rule semantics and report schema need no change,
  or change only through a documented schema version bump.

## Initial FAT rule set

The rules are cited by structure and field name from the Microsoft FAT
specification; `test/spec/lib/fsimg_spec/fat/rules.rb` registers them. Every
rule reads the boot sector, which the reader checks when it opens the volume,
so every scope is `open`. The category column names the policy when it is not
`must_reject`. The column "fsimg (observed)" records what the S6 witnesses
observed. A witness of a rule whose policy is not `must_accept` runs `info`,
the first stage that checks its rule. A valid witness, or one of a `must_accept` rule, runs every
reading stage. On an unmutated base, each stage asserts acceptance, and `ls`,
`extract` and `verify` compare their records with the base's known tree; a
mutated base leaves the directory, file and verification scopes unclassified,
so there `ls`, `extract` and `verify` assert only that they run without a
fault. All 139 deterministic witnesses agree with the expectations their rules
derive.

| Rule ID | Requirement | Category | fsimg (observed) |
| --- | --- | --- | --- |
| `fat.boot.present` | The image holds the 512-byte boot sector | normative | `info`: `EFORMAT` for FAT12 images cut to 0, 36, 62 and 511 bytes and a FAT32 image cut to 90 |
| `fat.boot.jump` | `BS_jmpBoot` is `EB ?? 90` or `E9 ?? ??` | normative | `info`: `EFORMAT` (no claim) |
| `fat.boot.signature` | `0x55 0xAA` at offsets 510 and 511 | normative | `info`: `EFORMAT` |
| `fat.bpb.bytes_per_sector` | 512, 1024, 2048 or 4096 | normative | `info`: `EFORMAT` |
| `fat.bpb.sectors_per_cluster` | Power of two, 1-128 | normative | `info`: `EFORMAT` |
| `fat.bpb.reserved_sectors` | Nonzero | normative | `info`: `EFORMAT` |
| `fat.bpb.num_fats` | Nonzero | normative | `info`: `EFORMAT` for zero |
| `fat.bpb.num_fats.supported` | fsimg reads one or two FATs | profile (`optional_feature`) | `info`: `EUNSUPPORTED` for 3, 254 and 255 |
| `fat.bpb.media` | `0xF0` or `0xF8`-`0xFF` | normative | `info`: `EFORMAT` |
| `fat.bpb.total_sectors` | Exactly one of `BPB_TotSec16` and `BPB_TotSec32` is nonzero; FAT32 uses `BPB_TotSec32` | normative | `info`: `ECORRUPT`; a FAT12 volume that counts only in `BPB_TotSec32` is accepted |
| `fat.bpb.fat_size` | `BPB_FATSz16` nonzero for FAT12/16 and zero for FAT32 | normative | `info`: `ECORRUPT` |
| `fat.bpb.root_entries` | `BPB_RootEntCnt` zero for FAT32 and nonzero for FAT12/16 | normative | `info`: `ECORRUPT` |
| `fat.bpb.root_entries.aligned` | On FAT12/16, the root region fills whole sectors | advisory (`must_accept`) | Accepted by `info`; the root region is rounded up |
| `fat.geometry.regions` | The total sector count includes the reserved area, the FATs and the root region | normative | `info`: `ECORRUPT` |
| `fat.geometry.data_region` | Where the regions fit, at least one data cluster follows them | profile | `info`: `ECORRUPT` |
| `fat.geometry.within_image` | The declared volume fits within the image or slice | normative | `info`: `ECORRUPT` |
| `fat.geometry.fat_capacity` | Each FAT holds `CountOfClusters + 2` entries | normative | `info`: `ECORRUPT` |
| `fat.geometry.max_cluster` | The largest cluster number stays below the bad-cluster marker | normative (`safety_only`) | `info`: `ECORRUPT` on its one witness, which also exceeds the image and its FAT |
| `fat.geometry.forced_variant` | A format forced with `-t` matches the variant the cluster count selects | profile | `info`: `EFORMAT` on a mismatch; a matching `-t` is accepted |
| `fat32.bpb.ext_flags` | When mirroring is disabled, the active FAT (bits 0-3) is less than the FAT count | normative | `info`: `ECORRUPT`; mirrored mode ignores the active nibble |
| `fat32.bpb.ext_flags.reserved` | `BPB_ExtFlags` bits 4-6 and 8-15 are zero | profile | `info`: `ECORRUPT` |
| `fat32.bpb.fs_version` | `BPB_FSVer` is `0:0` | normative (status `EUNSUPPORTED`) | `info`: `EUNSUPPORTED` |
| `fat32.bpb.reserved` | `BPB_Reserved` (bytes 52-63) is zero | normative (status `EUNSUPPORTED`) | `info`: `EUNSUPPORTED` |
| `fat32.bpb.root_cluster` | 2 to the maximum cluster | normative | `info`: `ECORRUPT` |
| `fat32.bpb.fsinfo_sector` | `BPB_FSInfo` is `0xFFFF` or a reserved-area sector after the boot sector | normative | `info`: `ECORRUPT` for 0, 32 of 32 reserved sectors, and `0xFFFE` |
| `fat32.bpb.fsinfo_sector.recommended` | `BPB_FSInfo` is 1 | advisory (`must_accept`) | Accepted by `info` at 2 and 31 |
| `fat32.bpb.fsinfo_sector.absent` | `BPB_FSInfo` is not `0xFFFF` | advisory (`must_accept`) | Accepted by `info` as no FSInfo |
| `fat32.bpb.backup_boot_sector` | `BPB_BkBootSec` is 0, `0xFFFF` or a reserved-area sector after the boot sector | normative | `info`: `ECORRUPT` for 32 of 32 reserved sectors and `0xFFFE` |
| `fat32.bpb.backup_boot_sector.recommended` | `BPB_BkBootSec` is 0 or 6 | advisory (`must_accept`) | Accepted by `info` at 29 |
| `fat32.bpb.backup_boot_sector.absent` | `BPB_BkBootSec` is not `0xFFFF` | advisory (`must_accept`) | Accepted by `info` as no backup |

Twelve well-isolated rules are worth more than thirty unverified ones.

The rule set reads the specification as follows where it is not decisive:

* **Cluster size.** The specification states no limit on bytes per cluster,
  so no rule bounds it. The valid witness `fat.valid.cluster_bytes_524288`,
  4096-byte sectors in 128-sector clusters, is accepted by `info`, and every
  reading stage runs without a fault.
* **Variant thresholds.** The cluster count alone selects the variant. Valid
  witnesses at 4084, 4085, 65524 and 65525 clusters are accepted by `info`,
  and every reading stage runs without a fault; `fat.geometry.forced_variant`
  covers only a forced `-t`.
* **Maximum cluster.** The specification says no FAT32 volume *should* hold
  allocatable cluster numbers at or above `0x0FFFFFF7`, so a reader may refuse
  such a volume or exclude those clusters from allocation, and the rule is
  `safety_only`. fsimg refuses it with `ECORRUPT`. The rule has no isolating
  witness: one that fits its image and its FAT needs a sparse image of about
  128 GiB.
* **Structure pointers.** Section 3.3 calls `BPB_FSInfo` "usually 1" and
  `BPB_BkBootSec` "0 or 6". Section 5 says FSInfo "must be located at sector
  #1", and section 3.6 that sector 6 "must contain a copy of the BPB". The rule
  set reads both as requirements of formatting: a reader follows the pointer,
  so another reserved-area sector is advisory and fsimg must accept it. The
  recommended values are those of section 3.3: FSInfo at 1, and the backup
  boot record at 6 or, with 0, none.
* **Reserved `BPB_ExtFlags` bits.** Section 3.3 calls bits 4-6 and 8-15
  reserved without saying what they hold, unlike `BPB_Reserved`, which "must
  be set to 0x0". The rule that they are zero is therefore the reader's
  profile, `fat32.bpb.ext_flags.reserved`, and the normative
  `fat32.bpb.ext_flags` covers only the active FAT.
* **Absent structures.** The specification gives `0xFFFF` in either pointer
  no meaning. Compatible readers, and fsimg, take it to mean the structure is
  absent, so the `.absent` rules are advisory and must accept.
* **Backup boot record.** Section 3.6 places a copy of the whole three-sector
  boot record at `BPB_BkBootSec`, but a reader uses only its first, the BPB
  copy. The rule requires only that sector inside the reserved area; the rest
  of the record is formatting. fsimg therefore accepts `BPB_BkBootSec` 30 with
  32 reserved sectors.
* **FAT32 backup copies.** A FAT32 witness's boot-sector mutations are
  mirrored into its backup boot sector, so each witness changes one field of
  the one logical boot record, never the agreement between the copies.
* **Reserved FAT entries.** `fat.fat.media_entry` (`FAT[0]` low byte equals
  `BPB_Media`) and `fat.fat.eoc_entry` (`FAT[1]` holds an end-of-chain value,
  with the FAT16/32 flag bits separate) have scope `verification`. Until the
  model decodes the FAT, every case leaves that scope unclassified, so both
  rules belong to milestone B2. FSInfo contents and the backup boot record's
  agreement with the primary are B2 rules for the same reason.

## Expected early findings

* **Strict verification fields.** The B2 witnesses of the fields the
  [stage policy](../specs/design-spec-testing.md#stage-policy) places in
  scope `verification` confirm that `info`, `ls` and `extract` accept them.
  The `FAT[0]` and `FAT[1]` checks at open are expected to fail this until
  the `TODO.md` item lands. The S6 boot-sector and geometry witnesses found no
  disagreement with fsimg.
* **Writer boundary interoperability.** The writer stays 16 clusters away from
  the variant cutovers unless `-s` is given. B1 checks the exact-threshold
  images that `-s` permits against fsck and mtools, which may themselves
  disagree. Disagreements are recorded per tool, not resolved by majority.

## Risks

| Risk | Mitigation |
| --- | --- |
| The rules and fsimg share a wrong interpretation | Rules, layouts and the builder are written from the specification, never ported from `lib/formats/fat/` or `test/fat.c`. S3, S5 and S6 reviews check this. External writers, readers and checkers cross-check. |
| Valid noncanonical images are labelled invalid | Normative, profile and advisory categories are kept separate; `safety_only` and `diagnostic_only` are used where the specification is not decisive |
| One field change silently breaks others | Every case re-evaluates the whole registry and records the true violation set |
| Shrinking turns a valid witness invalid | Shrinkers preserve validity by construction, and the rules are rechecked after every step |
| Random draws miss boundaries | Boundaries are enumerated deterministically in every profile |
| Reference tools disagree | Versions and results are recorded per tool; the specification decides where it is clear; divergences are triaged |
| The runner hangs or leaks processes | Process-group termination, wall, CPU, memory and output limits, isolated directories, and stub tests in S4 |
| Upgrades break reproducibility | Pinned gems, recorded tool versions, explicit recipes and promoted regression cases |
| The framework grows ahead of need | Only abstractions FAT exercises; generality is proved by the exFAT port |
| A second scripting language | All Ruby stays under `test/spec/` behind one wrapper with the existing skip convention |
| Budget creep in `make check` | S6 measures the quick profile, with the smoke-subset split as the fallback |
| Image generation dominates run time | Random draws mutate shared cached bases and are classified in memory; only a sampled subset is written and run through fsimg. S5 measures materialization and copy costs. |
| The harness becomes harder to understand than the code it tests | Review each stage for whether a filesystem implementer can read a rule, its witnesses and a failure record without learning harness internals |

## References

* Microsoft FAT specification:
  <https://www.scs.stanford.edu/~zyedidia/docs/_other/fat.pdf>. Rules cite its
  BPB, FAT type determination, FAT entry, FSInfo, directory entry and long
  name sections.
* PropCheck: <https://github.com/Qqwy/ruby-prop_check>, generator API at
  <https://www.rubydoc.info/github/Qqwy/ruby-prop_check/PropCheck/Generators>.
* BinData: <https://github.com/dmendel/bindata>.
* Minitest: <https://github.com/minitest/minitest>.
* dosfstools (`mkfs.fat`, `fsck.fat`): <https://github.com/dosfstools/dosfstools>.
* GNU mtools: <https://www.gnu.org/software/mtools/manual/mtools.html>.
* LLVM libFuzzer: <https://llvm.org/docs/LibFuzzer.html>, and structure-aware
  fuzzing at
  <https://github.com/google/fuzzing/blob/master/docs/structure-aware-fuzzing.md>.
* Related approaches, which inform the design but are not dependencies:
  - EverParse: <https://project-everest.github.io/everparse/>
  - FormatFuzzer: <https://github.com/uds-se/FormatFuzzer>
  - Kaitai Struct: <https://kaitai.io/>
  - Janus: <https://github.com/sslab-gatech/Janus>
  - Hydra: <https://github.com/sslab-gatech/hydra>
