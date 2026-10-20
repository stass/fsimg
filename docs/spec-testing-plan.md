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
  [fixtures](#fixtures-and-the-independent-builder).
* A solver dependency in development or CI; see
  [constraint solving](#constraint-solving).
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
| CLI stages | `info`, `ls -r`, `cat`, `extract -C`, `verify [-d]`, `create` (`specs/design-cli.md`) | The runner drives these. With `-j`, they emit NDJSON `summary` and `error` records. Each error record carries the integer status `code` (an `fsimg_status_t` value) and a prose `status` string. |

## Decisions

S0 records these decisions in the design document.

| Question | fsimg constraint | Decision |
| --- | --- | --- |
| Location | Tests live in `test/`. | `test/spec/`, with Ruby module `FsimgSpec`. |
| Task runner | `make` owns every test entry point (`check`, `check-extended`, `check-fuzz` and others). | No Rakefile. `make check-spec`, `check-spec-extended` and `check-spec-deep` call `test/spec.sh`, which runs `test/spec/run.rb --profile=NAME`. |
| Plain `make check` | `test/cli.sh` sets the precedent of an optional interpreter. | The quick profile is a `TESTS` member. It skips with 77 when Ruby or the gems are absent, and CI installs them so CI never skips it. |
| Ruby and gem versions | Ruby 4.0 releases bundle minitest 6. Ubuntu 24.04 ships Ruby 3.2. | The floor is Ruby 3.2, for `Data.define`. Lock the newest minitest that runs on both 3.2 and 4.0, plus prop_check 1.0.x and bindata 3.0.x. |
| Stages | `info` performs both the probe and the header-only open. The probe's no-claim result is `FSIMG_EFORMAT`; a claim that fails to open is `ECORRUPT` or `EUNSUPPORTED`. Full structural checks run only under `verify`: crosslinks, unowned allocations, FSInfo, the backup boot sector and full mirror comparison. | Stages are `info`, `ls`, `extract`, `verify`, `verify-deep` (`-d`) and `create`. `EFORMAT` separates "not claimed" from "claimed but rejected", so no separate probe command is needed. For FAT, `verify` is the only stage that sees most allocation-graph rules. |
| Outcome source | The CLI exits 0, 1 or 2. Corruption, unsupported and I/O failures all exit 1. | Classify from the `-j` error record's integer `code`, mapped to symbolic names through the enumeration in `include/fsimg.h`. The prose `status` string is never matched, because its wording is not an interface. Exit 2 is a harness bug, never an fsimg rejection. |
| Valid but unsupported | Principle 6 and the acceptance contract require a specific error for an unsupported feature. | A valid but unsupported image expects `FSIMG_EUNSUPPORTED`. |
| Strict verification | Full verification reports corruption for: <br>- a stale FSInfo free count; <br>- `FAT[1]` dirty or I/O-error flags; <br>- a diverging backup boot sector; <br>- a label that differs from the root label entry. <br>Header-only open rejects a `FAT[0]` media mismatch. | `verify` stays strict: these are `profile` rules that expect `verify` to reject. `info`, `ls` and `extract` must accept advisory or recoverable divergence. The `FAT[0]` check therefore moves from open to `verify` (`TODO.md`). |
| More than two FATs | The specification allows any nonzero `BPB_NumFATs`. fsimg reads one or two and reports a larger count as `FSIMG_EUNSUPPORTED`. | `fat.bpb.num_fats` is an `optional_feature` rule above two. Reading and emitting more than two FATs is a planned feature (`TODO.md`). When it lands, only the rule's class and policy change, and the generated expectations follow. |
| Base fixtures | No binary fixtures are committed (`fat-integration.sh.in`). Formatter output differs between tool versions. | Bases are recipes. Their images are cached in the build directory, keyed by the recipe and the tool versions. The manifest records the hash per tool version, and replay compares it when the versions match. |
| Fixture manifest | `specs/design-formats.md` requires a manifest for every generated image (format, geometry, profile, provenance, creation command, validator results). | The harness writes manifests in that shape and defines no second format. |
| Sanitizers | `check-asan`, `check-ubsan`, `check-lsan` and `check-msan` rebuild and run `make check` in a separate tree (`build-aux/sanitize.sh`). LSan and MSan run on Linux and report a skip elsewhere. | The quick profile runs under every sanitizer build through `TESTS`. No new sanitizer target is added. |
| Second format | exfatprogs already runs in CI. | Milestone D ports the harness to exFAT; QNX6 follows. |

## Harness architecture

### Three kinds of specification

The harness keeps three kinds of statement separate:

* **Physical layout:** bytes, offsets, endianness, bitfields, record lengths
  and conditional structures. These are BinData records and `pack`/`unpack`
  directives.
* **Logical validity:** cross-field geometry, reserved regions, cluster
  chains, directories, names, file sizes and allocation graphs. These are
  rule predicates over a decoded model.
* **Expected behaviour:** what fsimg must do when a rule holds or fails, and
  at which stage. These are rule policies.

Layout code never rejects a value. Invalid values must be representable so
they can reach fsimg. Format conformance and fsimg's supported subset are
recorded separately, as described under [rules](#rules-and-expectations).

### Tree layout

```text
test/spec.sh.in               wrapper: tool detection, exit 77, env, profile
test/spec/
  Gemfile, Gemfile.lock       minitest, prop_check, bindata (pinned)
  run.rb                      loads the suite for a profile, writes reports
  lib/fsimg_spec.rb
  lib/fsimg_spec/
    rule.rb                   Rule, Domain, Expectation; registry lookup
    case.rb                   Case recipe (JSON schema 1), Mutation list
    image.rb                  sparse materialization, mutation application
    runner.rb                 the only code that knows fsimg CLI syntax
    tree.rb                   expected/extracted tree model and digests
    fixtures.rb               base recipes, cache, manifests, tool adapters
    report.rb                 coverage JSON/text, failure directories
    fat/
      layout.rb               BinData: common BPB, FAT12/16 and FAT32
                              extensions, FSInfo, directory entry
      model.rb                total decoder: bytes -> model (missing fields
                              are explicit, never exceptions)
      geometry.rb             derived quantities over a model
      rules.rb                FAT rule registry with provenance
      builder.rb              independent minimal FAT image builder
      generators.rb           deterministic and PropCheck case generators
  suite/
    core/test_*.rb            harness self-tests (no fsimg needed)
    fat/test_*.rb             FAT layout, rules and end-to-end tests
  cases/fat/*.json            promoted regression recipes
  bin/replay, bin/report, bin/export-seeds
```

Start with these files and split one only when it grows past one
responsibility. Do not create empty modules for symmetry.

Generated output goes to the build directory, never to the source tree, so
VPATH and `distcheck` builds work. Images are cached in `test/spec-cache/`;
cases, reports and failure directories go to `test/spec-results/`.

### Data flow

1. **Base fixture.** A recipe names a geometry, a builder (`mkfs.fat`,
   `newfs_msdos` or the independent `builder.rb`) and an optional tree. It is
   materialized once per tool-version key. The harness decodes the result,
   checks that the requested geometry and variant were actually produced,
   and records the read-only checker's verdict in the manifest.
2. **Case.** A case is a base plus a list of [mutations](#mutations). It is
   applied to a sparse-preserving copy.
3. **Classification.** `fat/model.rb` decodes the mutated image
   independently, and every applicable rule is evaluated. The result is the
   case's specification validity: `valid`, `invalid(rules)` or
   `valid_unsupported(rules)`. It also says whether the case is
   `single_rule` or `multi_rule`.
4. **Expectation.** The expected fsimg outcome at each stage is derived from
   the violated rules' policies. Generated cases have no hand-written
   expectation.
5. **Execution.** The runner executes the stages and classifies each outcome.
6. **Comparison.** The harness compares observed with expected outcomes,
   compares extracted trees with the model's tree, and records the oracles'
   observations.
7. **Reporting.** Coverage counters are updated. Any mismatch writes a
   [failure record](#failure-records).

### Rules and expectations

A rule has the following fields:

* **`id`:** a stable identifier such as `fat.bpb.bytes_per_sector`. It is the
  regression identifier, so it never changes meaning.
* **`applies_to`:** a predicate over the model, covering the variant, the
  feature flags and any preconditions.
* **`predicate`:** a pure Ruby function over the decoded or constructed
  model. It never calls fsimg.
* **`domain`:** legal values, explicit invalid examples and boundaries, or a
  mutation factory for relational rules.
* **`class`:**
  - `normative` states the published format;
  - `profile` states fsimg's supported subset or its strict verification
    policy;
  - `advisory` covers hint fields.

  Profile rules are reported separately, so fsimg policy never rewrites the
  specification.
* **`policy`:** one of the behaviour policies below.
* **`detect_at`** and **`status`:** the first stage that must refuse a
  violation, and the fsimg status it reports. An example is `info`,
  `FSIMG_ECORRUPT`.
* **`source`:** the specification structure, field and section, plus any
  compatibility note. Normative text and observed tool behaviour are kept
  apart.
* **`related`** (optional): correlated rules, used to explain unavoidable
  multi-rule violations.

| Policy | Meaning | Example |
| --- | --- | --- |
| `must_accept` | A valid, supported image reads and extracts correctly | FAT32 with a populated tree |
| `must_reject` | A violation is refused with `status` at `detect_at` | Zero bytes per sector |
| `safety_only` | Either result is allowed; it must never crash, hang or write outside the destination | Cross-linked chains at `extract` |
| `equivalent_contents` | Different legal encodings give the same extracted tree | A relocated chain |
| `optional_feature` | Valid but outside fsimg's support; expects `FSIMG_EUNSUPPORTED` at `detect_at` | `BPB_NumFATs` above two; FAT32 revision other than 0:0 |
| `diagnostic_only` | Disagreement is recorded, not failed | A field no stage checks |

The expected outcome at each stage follows from the violated rules:

* **No violation:** every stage accepts, and the extracted tree equals the
  model's tree.
* **`must_reject` or `optional_feature` with `detect_at` = S:**
  - stages before S accept;
  - S and every later stage reject with `status`;
  - `info` precedes `ls`, which precedes `extract` and then `verify`.
* **`safety_only`:** any accept or reject is allowed. A crash, timeout,
  sanitizer report, output-limit breach or unsafe extraction is a failure.
* **Several violations:**
  - the earliest `detect_at` decides the first rejecting stage;
  - the allowed statuses are those of the rules detected at that stage;
  - this deliberately avoids encoding fsimg's internal check order.

Every case, whatever its policy, also asserts:

* bounded execution;
* no sanitizer report;
* safe extraction: only regular files and directories, nothing outside the
  destination, and no symbolic links unless the format and options create
  them.

### Case classes

* **A. Valid.** Generate a consistent geometry and tree, materialize them,
  check every applicable rule, then assert fsimg's acceptance and the
  extracted content.
* **B. Targeted invalid.** Start from a validated base, change one value or
  relationship, and re-evaluate the whole registry.
  - If only the intended rule fails, the case is `single_rule`.
  - If dependent rules fail with it, the case is `multi_rule`, and the actual
    set of violated rules is recorded.
  - A clean single-rule claim is never made for a mutation that also
    invalidates dependent geometry.

  This class is what distinguishes the harness from byte fuzzing: its
  failures explain themselves.
* **C. General malformed.** Combined mutations, truncations and arbitrary
  byte edits. Only safety is asserted. Acceptance or rejection is recorded but
  never taken as evidence of compliance.

### Case recipes

A case is a versioned JSON recipe:

```json
{
  "schema_version": 1,
  "id": "fat32.bpb.bytes_per_sector.invalid.1000",
  "format": "fat32",
  "profile": "baseline",
  "base_id": "fat32-64mib-512b-1spc",
  "base_sha256": "<hash recorded at materialization>",
  "tool_versions": {"mkfs.fat": "4.2", "mtools": "4.0.49"},
  "stages": ["info", "ls", "extract", "verify"],
  "mutations": [
    {"op": "set_u16le", "offset": 11, "value": 1000}
  ],
  "intended_violation": "fat.bpb.bytes_per_sector",
  "actual_violations": ["fat.bpb.bytes_per_sector"],
  "isolation": "single_rule",
  "expected": {"info": "reject:FSIMG_EFORMAT"},
  "source": "Microsoft FAT specification, BPB_BytsPerSec"
}
```

Optional fields are:

* `tree_id`, `source_revision` and `runner_version`;
* `random_seed`, `resource_limits` and `supports`.

The loader refuses a recipe whose hash is a placeholder rather than a real
SHA-256.

A random seed alone does not reproduce a case across PropCheck or formatter
upgrades. Recipes therefore carry explicit field values, graph operations and
the base identity.

### Mutations

Milestone A implements these mutations:

* `set_u8(offset, value)`;
* `set_u16le(offset, value)` and `set_u32le(offset, value)`;
* `replace_bytes(offset, bytes)`;
* `truncate(length)`.

Milestone B adds these:

* `set_fat_entry(table, cluster, value)`;
* `set_directory_entry(directory, index, fields)`;
* `splice_chain(file, next_cluster)`;
* `relocate_chain(file, clusters)`.

Every mutation checks its bounds and refuses offsets or values that do not fit
its target; it never wraps. No mutation consults the rules, so it can write
any invalid encoding. A single field patch writes bytes directly, without
building a BinData object. For example, offset 11 receives
`[value].pack("v")`.

### Runner contract

`runner.rb` is the only file that knows fsimg's command syntax. Each stage
maps to one command.

| Stage | Command |
| --- | --- |
| `info` | `fsimg info -j -i IMG [-t FORMAT]` |
| `ls` | `fsimg ls -r -j -i IMG` |
| `extract` | `fsimg extract -j -i IMG -C DEST` (`DEST` does not exist yet) |
| `verify`, `verify-deep` | `fsimg verify -j [-d] -i IMG` |
| `create` | `fsimg create -j -R -t FORMAT [-s BYTES] [-O sector=N] [-O cluster=N] -o IMG SRC` |

Every invocation also passes:

* `-m 64M`;
* `-l seconds=N` and `-l output=N`;
* `-T` pointing to a scratch directory inside the case directory.

The sandbox policy stays at the default, `auto`. Setting
`FSIMG_SPEC_SANDBOX=required` makes Linux and FreeBSD CI demand confinement.
`-S off` is never the default. The runner also enforces limits outside fsimg:

* `Process.spawn(..., pgroup: true)`, so the whole process group is killed on
  wall-clock expiry. `Timeout.timeout` alone is never relied on.
* `rlimit_cpu` and, where it works, `rlimit_as`. macOS does not enforce
  `RLIMIT_AS`, so on macOS the limit is reported as unavailable, not applied.
* stdout and stderr capture capped at a fixed number of bytes.

Each result records:

* the outcome class: `accepted`, `rejected(STATUS)`, `crashed`, `timed_out`,
  `output_exceeded`, `sanitizer_failure` or `harness_error`, where `STATUS`
  is the symbolic name of the error record's integer `code`;
* the exit status, signal and duration;
* the bounded output and the parsed NDJSON records;
* the extracted tree digest and the oracle observations.

An `exit 1` without exactly one `complete:false` error record is
`harness_error`, never a rejection. A sanitizer report is recognized by its
`ERROR: AddressSanitizer`, `runtime error:` or `LeakSanitizer` lines.

Running the CLI is the only adapter in milestone A. A native in-process batch
driver over `fsimg_reader_open`, `fsimg_reader_next`, `fsimg_verify` and
`fsimg_extract_at` is added only if profiling shows that process start-up
dominates the quick budget. The CLI path also exercises the worker, the
protocol and the sandbox, and the in-process path would not. Ruby FFI is not
used.

Host safety:

* Generated images are never mounted.
* External checkers never run on host block devices.
* Mutated images reach external tools only in the nightly and deep profiles,
  because a hostile image can attack an oracle too. Those runs use the same
  limits as fsimg and, where the host provides one, a low-privilege account
  or OS sandbox. The report marks every oracle run that was not confined.

### Fixtures and the independent builder

* **Formatter bases.**
  - Use `mkfs.fat --invariant -i <fixed id>` on macOS and Linux, and
    `newfs_msdos` on FreeBSD. A typical command is
    `mkfs.fat -C --invariant -F 32 -s 1 base.img 65536`, where the size is in
    1024-byte blocks.
  - Populate with `mmd` and `mcopy -m`, using source files whose times are
    fixed.
  - The harness rereads the BPB and refuses a base whose variant or geometry
    differs from the recipe. A formatter that silently substitutes another
    geometry never supplies a witness.
  - It records `fsck.fat -n` (or `fsck_msdosfs -n`) and an `mdir` listing in
    the manifest.
  - Whether mtools stamps directories with fixed times needs checking. If it
    does not, populated bases are compared by field and tree, not by image
    hash, and the manifest says so.
* **Independent builder** (`fat/builder.rb`).
  - It writes a sparse empty volume, and in milestone B a small tree of
    contiguous files, from an explicit geometry: boot sector and BPB, any
    number of FATs with reserved entries, the FAT12/16 root region or FAT32
    root cluster, FSInfo, and the backup boot sector.
  - It exists for geometries formatters will not produce: exact cluster
    counts of 4084, 4085, 65524 and 65525, one FAT or more than two, 4096-byte
    sectors at the variant thresholds, and clusters larger than 32 KiB. It
    also serves hosts without dosfstools.
  - It is authored from the Microsoft specification, not from
    `lib/formats/fat/` or `test/fat.c`. Only this keeps it independent
    evidence. Its output must pass the read-only checker and `mdir`, where
    they support the geometry, before fsimg sees it. Its cases are labelled
    `independent-construction`.
  - A valid image with a different geometry is always rebuilt. Rewriting
    `BPB_BytsPerSec` on a 512-byte-sector image does not make a valid
    4096-byte-sector image.
* **Copies.**
  - Each case writes into a sparse-preserving copy. The copy walks data
    regions with `IO::SEEK_DATA`/`IO::SEEK_HOLE` and falls back to `cp -c` on
    APFS or `--reflink=auto` on Linux.
  - A 64 MiB FAT32 base must not become 64 MiB of physical writes per case.
    S5 measures this.
* **Truncated input.** `model.rb` decodes any prefix, including fewer than 512
  bytes. BinData records are only read over complete structures, so a
  truncated boot sector reaches fsimg without the layout code raising first.

### Generation and shrinking

* **Deterministic cases are mandatory.** Every small legal domain, documented
  sentinel and threshold neighbour is enumerated in every profile:
  - the 4084/4085 and 65524/65525 cluster counts;
  - FAT capacity at an exact fit and one entry short;
  - the FAT entry markers.

  These cases do not depend on what the random draws produce.
* **PropCheck explores beyond those points.** It draws over a field's full
  width with `one_of`, `constant`, `choose`, `tuple`, `array`, `map` and
  `bind`.
  - Random invalid draws need no filter, because classification derives
    their expectation. Use `map` from legal primitives, never `.where` filters
    that reject most samples.
  - PropCheck generates **recipes**, not raw images, so shrinking yields
    meaningful minimal cases.
  - Example counts are explicit for each profile.
* **Domain-only properties never count as fsimg evidence.** A property that
  checks only Ruby predicates validates the test definitions. The report
  counts it separately from cases that ran fsimg.
* **Recipe shrinkers.** Milestone B adds shrinkers that:
  - remove unrelated files and directories;
  - shrink files and chains;
  - reduce a cycle to two clusters and a crosslink to two files sharing one
    cluster;
  - drop unnecessary mutations;
  - move values toward the nearest boundary without making the witness valid.

  Every shrink step re-checks the rules before fsimg runs.

### Coverage report

`report.rb` writes `test/spec-results/coverage.json` and a text summary. For
each rule the report records:

* the attempted, passed, failed and skipped counts;
* the boundaries exercised and the stages asserted;
* the isolation achieved: `single_rule`, `partial` or `multi_rule_only`;
* the oracle results.

It keeps these states distinct:

* rule declared but not exercised;
* valid witness available or unavailable;
* targeted invalid witness available, impossible, or not yet generated;
* behaviour asserted against fsimg, or only the Ruby predicate tested;
* fixture accepted by its formatter, or unsupported by that tool;
* oracle agrees, disagrees, not applicable, or unavailable.

A declared rule never counts as covered until fsimg has run its cases.

### Failure records

Every final mismatch writes this directory:

```text
test/spec-results/failures/<case-id>/
  case.json       versioned recipe and expectation
  fixture.json    base recipe, image hash, tool versions
  fsimg.stdout    bounded
  fsimg.stderr    bounded
  oracles.json    independent tool observations
  result.json     observed outcome per stage, timings, sanitizer state
  image.img       only within the size policy (sparse, at most 64 MiB logical)
  replay.sh       exact replay command, with no host-specific secrets
```

`bin/replay CASE.json` rebuilds and reruns one case. A confirmed defect is
promoted by copying its recipe into `test/spec/cases/fat/`, with its rule ID
and a one-line reason. The quick profile always replays every promoted case.
Recipes are preferred to committed images.

### Constraint solving

The harness has no solver. FAT geometry is generated constructively: choose
the sector size, cluster size, reserved count and table count, then compute
the dependent fields. A solver such as Z3 is reconsidered only after these are
documented:

* at least three recurring constraint families that matter and are hard to
  generate constructively;
* a measured coverage gain.

If one is added, it is held to the following:

* it is an optional `require` and runs in a subprocess;
* it solves only a dozen or so bounded scalar variables;
* each query has a time limit of 100-200 ms, and the PR total is about 2 s;
* its witnesses are cached as JSON, so CI never needs the solver;
* every witness is rechecked against the Ruby rules;
* `unsat` is distinguished from `unknown`.

When no single-rule violation is satisfiable, the target is recorded as
impossible, not forced.

## Build, dependencies and CI

* **configure.ac:** `AC_PATH_PROG([RUBY], [ruby])` and
  `AC_PATH_PROG([BUNDLE], [bundle])`, both optional, and `test/spec.sh` in
  `AC_CONFIG_FILES`.
* **`test/spec.sh.in`:**
  - It exits 77 with a `SKIP:` message when Ruby is missing or older than
    3.2, or when `bundle check` fails. The message names
    `bundle install --gemfile=test/spec/Gemfile`.
  - It sets `BUNDLE_GEMFILE` and `BUNDLE_FROZEN=true`, so a test run never
    rewrites the lock or reaches the network.
  - It exports `FSIMG_BIN`, the source and build directories, and the tool
    paths that `configure` found: `MKFS_FAT`, `FSCK_FAT`, `NEWFS_MSDOS`,
    `FSCK_MSDOSFS`, `MCOPY`, `MMD`, `MDEL` and `FATCAT`.
  - It runs `run.rb --profile=${FSIMG_SPEC_PROFILE:-quick}`.
* **Makefile.am:**
  - `test/spec.sh` joins `TESTS`.
  - New targets: `check-spec` (quick), `check-spec-extended` (nightly) and
    `check-spec-deep` (opt-in).
  - `check-extended` gains `check-spec-extended`.
  - `test/spec/` goes into `EXTRA_DIST`.
* **ASCII gate:** `build-aux/check-ascii.sh` also scans `*.rb`, `*.json`,
  `Gemfile` and `Gemfile.lock`. Every Ruby file starts with the SPDX
  BSD-2-Clause header and `# frozen_string_literal: true`.
* **Licences:** minitest (MIT), prop_check (MIT, no dependencies) and bindata
  (BSD-2-Clause) are development tools. They are not runtime dependencies and
  are not vendored. GPL tools (dosfstools, mtools) run only as executables,
  and nothing is copied from them. Rule text, builders and format logic are
  written independently from published format documents.
  `docs/references.md` records the oracles.
* **CI** (`.github/workflows/check.yml`):
  - macOS: `brew install ruby`.
  - Ubuntu: `apt-get install ruby ruby-bundler`, giving Ruby 3.2.
  - FreeBSD: `lang/ruby3x` with bundler.
  - Every job runs `bundle install` with a cached path outside the checkout.
  - The jobs upload `test/spec-results/` with the logs.
* **docs/build.md** gains the setup, the profiles, the environment variables,
  the replay command and the report location. It also states the explicit
  skips per host, as it already does for the other oracles.
* **specs/style.md** gains a short Ruby section:
  - binary strings via `String#b`;
  - explicit-endian `pack` directives (`v`, `V`, `S<`, `L<`);
  - keyword arguments for case construction;
  - no `eval`, `instance_eval` DSLs or monkey patching;
  - two-space indentation;
  - the existing [readable tests](../specs/style.md#readable-tests) rules
    apply.

### Profiles and budgets

The budgets below are targets, measured in S6 and S7 and then adjusted.

| Profile | Contents | Target |
| --- | --- | --- |
| `quick` (`make check`, `check-spec`) | Harness self-tests; every deterministic boundary and domain witness; 100-300 cheap PropCheck examples; at most about 30 fsimg stage runs per variant; every promoted regression case | Adds at most about 60 s to `make check` on the macOS host. If it adds more, `make check` keeps a smoke subset and `check-spec` keeps the rest. |
| `nightly` (`check-spec-extended`) | Thousands of random examples, 50-150 oracle cross-checks, class C cases | 10-30 min |
| `deep` (`check-spec-deep`) | Large volumes, broad mutation matrices, every oracle on every case | Explicit opt-in |

Every profile enforces ceilings on image capacity, file count, path depth,
memory, output and per-case run time. The smallest FAT32 volume has 65,525
clusters, so sparse logical images of tens of MiB are routine even when few
bytes are written.

The sanitizer runs come from `check-asan`, `check-ubsan`, `check-lsan` and
`check-msan` executing `quick` inside their own trees. Fuzzing stays in `check-fuzz`, which uses the seeds
exported in S8.

## Work packages

Milestone A is S0-S8. Every stage leaves `make check` passing and follows the
review and commit gates.

### S0. Design document and indices

* Write `specs/design-spec-testing.md`, the target design in the present
  tense, from the [architecture](#harness-architecture) and
  [decisions](#decisions) above. It covers:
  - the three kinds of specification;
  - the rule fields, policies and expectation derivation;
  - the case classes and recipe schema 1;
  - the runner contract and outcome classes;
  - the fixture, manifest and independence rules;
  - the coverage states.
* Reference it from `specs/README.md` and from a short testing section in
  `specs/architecture.md`. That section records only the cross-subsystem
  decision: that the harness's contract is the integer status `code` of
  CLI `-j` error records.
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

* `Rule`, `Domain`, `Expectation`, `Case` and the milestone-A
  [mutations](#mutations), with sparse application in `image.rb`.
* Self-tests cover:
  - Mutations: bounds checks reject offsets and values outside the target.
  - Case JSON: it round-trips exactly, and a placeholder hash is rejected.
  - Expectations: derivation over synthetic rule sets covers single and
    multiple violations, `detect_at` ordering and status sets.
  - Copies: a sparse-preserving copy keeps its holes on APFS and ext4. If a
    host cannot verify this, the test reports a skip.
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
* `model.rb` decodes any byte prefix into a model with explicit `missing`
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
* **Gate:** the layout offsets are reviewed against the specification text.
  No PropCheck run makes the geometry code raise.

### S4. Runner

* Implement the [runner contract](#runner-contract) and `tree.rb`. `tree.rb`
  walks an extraction directory with `File.lstat`, never following links, and
  records per entry the path, type, size and SHA-256. It flags anything that
  is not a regular file or directory.
* Self-tests use stub executables written into the test's temporary
  directory. Each stub demonstrates one failure:
  - it never exits;
  - it forks a child that never exits;
  - it floods stdout;
  - it dies by `SIGSEGV`;
  - it prints a fake sanitizer report;
  - it exits 1 with no error record;
  - it emits malformed NDJSON;
  - it writes a symbolic link and a file outside the destination.

  Each must be classified correctly, and no process may survive.
* Find out how a worker that dies by a signal, or under a sanitizer, reaches
  the coordinator's exit and error record (`cmd/fsimg/`). The classifier
  must map that path to `crashed` or `sanitizer_failure`, never to a clean
  rejection. If the coordinator hides it, fix the coordinator with a failing
  test first, or record it in `TODO.md`.
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
* Add the cache and the manifests.
* Add the independent empty-volume builder.
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
* Run cases end to end through the stages, using the first rejecting stage
  and the content stages, within the quick budget.
* Inventory the existing `test/fat.c` corruption and detection cases against
  the rules. Every gap either becomes a Ruby case or is noted in the design
  as intentionally white-box.
* Validate the interfaces against injected faults before expanding. For
  example, temporarily mis-set a rule's `detect_at`, or patch a local fsimg
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
  or a policy question for the design document.

### S7. Reports, failures and regressions

* Implement the [coverage report](#coverage-report),
  [failure records](#failure-records), `bin/replay`, `bin/report` and the
  promotion procedure. `spec.sh` prints the text summary at the end of a run.
* **Gate:**
  - the report shows the real counts for the S6 run;
  - an injected fault produces a failure record whose `replay` reproduces it;
  - at least one minimized negative recipe is promoted.

### S8. Fuzz seeds and sanitizer pass

* `bin/export-seeds DIR` writes the deterministic invalid witnesses and the
  promoted cases as fuzz seeds. FAT32 seeds are cut the way
  `test-fat --seed-variant` cuts them (data offset plus 16 clusters) to fit
  the 3 MiB maximum length.
* `build-aux/fuzz.sh` adds these seeds to `fuzz-corpus/` when Ruby is
  available, and is otherwise unchanged. The native harness never
  calls Ruby inside a fuzzing iteration.
* Run `check-asan`, `check-ubsan`, `check-lsan` and `check-msan` on the
  final milestone tree with the spec suite inside. Record per host in build.md which sanitizer checks ran
  and which were skipped.
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

* **B1, tree model and cross-oracles.**
  - Generate small trees with PropCheck: names that are 8.3, long, Unicode
    BMP and non-BMP, or reserved; nesting depth; empty, 1-byte,
    cluster-minus-one, exact-cluster and multi-cluster files; bounded
    directory width.
  - External create, fsimg read: mtools populates, then fsimg `extract`
    output must equal the model in path, type, size and SHA-256.
  - fsimg create, external read: `fsimg create -R`, then `mcopy -s` must
    reproduce the tree, with `fsck -n` and fatcat as further evidence.
  - `fsimg create` followed by `fsimg extract` alone is never evidence,
    because both paths can share a bug.
  - Exercise writer geometry rules with `-s`, `-O sector=` and `-O cluster=`,
    including exact-threshold sizes, which the writer permits only with `-s`.
  - Then decide whether the fixed cases in `fat-integration.sh` move into the
    harness. Moving them is worthwhile only if it removes a mechanism.
* **B2, semantic mutations.**
  - `model.rb` gains a bounded FAT-table and directory decoder for small
    images.
  - The mutations are the milestone-B [mutations](#mutations), plus LFN
    order, checksum and UTF-16 faults, FSInfo signatures and hints, backup
    boot divergence, and mirror divergence.
  - Expected stages follow fsimg's layered checks:
    - directory-chain faults reject at `ls`;
    - file-chain faults reject at `extract`;
    - crosslinks, unowned allocations and owned-free clusters reject only at
      `verify`;
    - `extract` of a crosslinked image is `safety_only`;
    - strict-verification fields are `profile` rules that reject at
      `verify`.
* **B3, metamorphic checks.**
  - Relocating a chain leaves the extracted tree unchanged and `verify`
    accepting.
  - Changing the FAT32 high nibble leaves the extracted tree unchanged; its
    `verify` outcome is recorded under the strict-verification policy.
  - Setting the FSInfo hint to unknown (`0xFFFFFFFF`) leaves every stage
    accepting.
  - Directory-slot reordering and physical creation order do not change the
    extracted tree.
  - Changing boot code or a permitted label changes nothing beyond what the
    rules allow.
  - Every transformation is itself checked against the rules before it is
    used.
* **B4, recipe shrinkers,** as described under
  [generation and shrinking](#generation-and-shrinking).
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
* QNX6 follows. Its valid images come mainly from the builder, and the
  QNX 6.5.0 VM harness (`docs/qnx6-infrastructure.md`) serves as a manual
  oracle. The harness can then serve the QNX6 writer's acceptance evidence.
* **Gate:** the core runner, rule semantics and report schema need no change,
  or change only through a documented schema version bump.

## Initial FAT rule set

The rules are cited by structure and field name from the Microsoft FAT
specification. The column "fsimg (hypothesis)" comes from reading
`lib/formats/fat/codec.c` and `read.c`. It is a hypothesis that S6 confirms
or corrects; it is not an assertion.

| Rule ID | Requirement | Class | fsimg (hypothesis) |
| --- | --- | --- | --- |
| `fat.boot.jump` | `BS_jmpBoot` is `EB ?? 90` or `E9 ?? ??` | normative | `info`: `EFORMAT` (no claim) |
| `fat.boot.signature` | `0x55 0xAA` at offsets 510 and 511 | normative | `info`: `EFORMAT` |
| `fat.bpb.bytes_per_sector` | 512, 1024, 2048 or 4096 | normative | `info`: `EFORMAT` |
| `fat.bpb.sectors_per_cluster` | Power of two, 1-128 | normative | `info`: `EFORMAT` |
| `fat.bpb.cluster_bytes` | Bytes per cluster above 32 KiB is legal but discouraged | advisory | Accepted up to 512 KiB |
| `fat.bpb.reserved_sectors` | Nonzero | normative | `info`: `EFORMAT` |
| `fat.bpb.num_fats` | Nonzero per the specification; fsimg supports 1-2 | normative and profile | `info`: `EFORMAT` for zero; `EUNSUPPORTED` above two |
| `fat.bpb.media` | `0xF0` or `0xF8`-`0xFF` | normative | `info`: `EFORMAT` |
| `fat.bpb.total_sectors` | `BPB_TotSec16` or `BPB_TotSec32` selection; not both nonzero | normative | `info`: `ECORRUPT` |
| `fat.bpb.fat_size` | `BPB_FATSz16` nonzero for FAT12/16 and zero for FAT32 | normative | `info`: `ECORRUPT` |
| `fat.bpb.root_entries` | Zero for FAT32; nonzero for FAT12/16; aligned to sector size | normative | `info`: `ECORRUPT` for a zero or nonzero count in the wrong variant; alignment is rounded up, not checked |
| `fat.geometry.data_region` | Data region is positive | normative | `info`: `ECORRUPT` |
| `fat.geometry.within_image` | Declared volume fits within the image or slice (truncation) | normative | `info`: `ECORRUPT` |
| `fat.geometry.fat_capacity` | FAT holds `clusters + 2` entries | normative | `info`: `ECORRUPT` |
| `fat.geometry.variant` | Variant follows the cluster-count thresholds; a forced `-t` must match | normative | Accepts all four threshold neighbours; mismatched `-t` gives `EFORMAT` |
| `fat.geometry.max_cluster` | Maximum cluster stays below the bad-cluster marker | normative | `info`: `ECORRUPT` |
| `fat.fat.media_entry` | `FAT[0]` low byte equals `BPB_Media` | profile (`verify`) | `info`: `ECORRUPT`; moves to `verify` (`TODO.md`) |
| `fat.fat.eoc_entry` | `FAT[1]` holds an end-of-chain value; the FAT16/32 flag bits are separate | normative | `info`: `ECORRUPT` on the value; flags only at `verify` |
| `fat32.bpb.ext_flags` | Reserved bits are zero; the active FAT is less than the FAT count | normative | `info`: `ECORRUPT` |
| `fat32.bpb.fs_version` | `BPB_FSVer` is `0:0` | normative | `info`: `EUNSUPPORTED` |
| `fat32.bpb.reserved` | `BPB_Reserved` (bytes 52-63) is zero | normative | `info`: `EUNSUPPORTED` |
| `fat32.bpb.root_cluster` | 2 to the maximum cluster | normative | `info`: `ECORRUPT` |
| `fat32.bpb.fsinfo_backup_placement` | `BPB_FSInfo` and `BPB_BkBootSec` lie within the reserved area; 6 is the recommended backup sector | normative and advisory | `info`: `ECORRUPT` when out of range |

Twelve well-isolated rules are worth more than thirty unverified ones. A rule
gets `must_reject` only once its source is written down.

## Expected early findings

* **Strict verification fields.** Under the
  [decision](#decisions), these fields are `profile` rules that `verify`
  rejects and the content stages accept:
  - the FSInfo free count;
  - the `FAT[1]` dirty and I/O-error flags;
  - backup boot divergence;
  - a label mismatch.

  The first witnesses confirm that `info`, `ls` and `extract` accept them.
  The `FAT[0]` media check at open is expected to fail this until the
  `TODO.md` item lands.
* **Writer boundary interoperability.** The writer stays 16 clusters away from
  the variant cutovers unless `-s` is given. B1 checks the exact-threshold
  images that `-s` permits against fsck and mtools, which may themselves
  disagree. Disagreements are recorded per tool, not resolved by majority.

## Risks

| Risk | Mitigation |
| --- | --- |
| The rules and fsimg share a wrong interpretation | Rules, layouts and the builder are written from the specification, never ported from `lib/formats/fat/` or `test/fat.c`. S3, S5 and S6 reviews check this. External writers, readers and checkers cross-check. |
| Valid noncanonical images are labelled invalid | Normative, profile and advisory classes are kept separate; `safety_only` and `diagnostic_only` are used where the specification is not decisive |
| One field change silently breaks others | Every case re-evaluates the whole registry and records the true violation set |
| Shrinking turns a valid witness invalid | Shrinkers preserve validity by construction, and the rules are rechecked after every step |
| Random draws miss boundaries | Boundaries are enumerated deterministically in every profile |
| Reference tools disagree | Versions and results are recorded per tool; the specification decides where it is clear; divergences are triaged |
| The runner hangs or leaks processes | Process-group termination, wall, CPU, memory and output limits, isolated directories, and stub tests in S4 |
| Upgrades break reproducibility | Pinned gems, recorded tool versions, explicit recipes and promoted regression cases |
| The framework grows ahead of need | Only abstractions FAT exercises; generality is proved by the exFAT port |
| A second scripting language | All Ruby stays under `test/spec/` behind one wrapper with the existing skip convention |
| Budget creep in `make check` | S6 measures the quick profile, with the smoke-subset split as the fallback |

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
