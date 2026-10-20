# Specification-driven testing

The specification-driven test harness checks fsimg against named rules taken
from published format specifications. It builds valid and targetedly invalid
images, runs the `fsimg` command on them stage by stage, and compares every
result with the outcome the rules require and with what independent tools
observe. Its distinctive output is **specification coverage**: for each rule,
the witnesses that reached fsimg, the boundaries and stages they exercised, the
independent tools' observations, and a recipe that reproduces each case. A
**witness** is a case that demonstrates a rule: a valid witness satisfies it,
and a targeted witness violates it.

The harness is black-box and independently authored. It adds to the white-box C
tests (`test/fat.c`, `test/fat-write.c`), the fixed external-oracle scripts
such as `test/fat-integration.sh` and the libFuzzer targets, and replaces none
of them; it never changes the comparison baseline. It serves FAT12, FAT16 and
FAT32, exFAT and QNX6. The core (rules, cases, runner, reports) is
format-independent; each format adds its own layouts, model, rules, builder and
tool adapters. The [plan](../docs/spec-testing-plan.md) orders the
construction.

## Data flow

One classification decides every expectation. The harness decodes each image
with its own model, evaluates every applicable rule, and derives what fsimg
must do at each stage from the violations it finds. No case carries a
hand-written expectation.

1. **Base.** A base recipe names a builder, its parameters and an optional file
   tree. The harness materializes it once per cache key and refuses it if the
   decoded geometry differs from the parameters.
2. **Case.** A reading case is a base plus ordered mutations, applied to an
   in-memory view; a creation case is a file tree plus creation settings.
3. **Classification.** The model decodes the case image and evaluates every
   applicable rule. The result is the set of violations, each a rule and the
   scope that must find it, and the scopes the model could not decode
   completely. The violations' policies give the allowed outcome at each stage.
4. **Execution.** The runner writes the image into a fresh case directory,
   executes the stages under fixed bounds and classifies each outcome.
5. **Comparison.** Outcomes are compared with the expectation, the records of
   accepting stages with the model, and the host state with the safety checks.
   Independent tools' observations are recorded.
6. **Report.** Coverage counters are updated, and a mismatch writes a failure
   record with a replay recipe.

Because classification is the only source of expectations, changing a rule's
domain, scope or policy changes every generated case and its expectation
without editing a per-case test. A builder, formatter or generator defect that
some rule detects shows up as an image meant to be valid that classifies
invalid, and is reported as a **harness defect**, never as an fsimg result.
Classification cannot report a violation for which no rule exists; fsimg then
rejects an image the harness expected it to accept, and triage records the
missing rule.

## Rules

The harness keeps three kinds of statement apart. **Physical layout** (bytes,
offsets, endianness, bitfields, record lengths) is expressed by BinData records
and explicit-endian `pack` directives. Layout code never rejects a value, so
every invalid encoding is representable and can reach fsimg, and a format's
layouts keep separate records for structures the specification makes mutually
exclusive, such as the FAT12/16 and FAT32 BPB extensions. **Logical validity**
(cross-field geometry, reserved regions, chains, directories, names, sizes,
allocation graphs) is expressed by rule predicates over the decoded model.
**Expected behaviour**, what fsimg must do about a violation and which check
must find it, is expressed by rule policies and scopes.

The model decoder is total. It decodes any byte prefix, including one shorter
than a boot sector, into a model whose absent fields are explicit `Undefined`
markers rather than exceptions; records are read only over complete structures.
Derived quantities never raise on a zero divisor, an overflow or contradictory
fields; they are undefined instead, and a rule applies only when every input
its predicate reads is present and defined. A separate rule states that a
structure is present. The model derives the FAT variant from the data-cluster
count, as the [format contract](design-formats.md#fat12-fat16-and-fat32)
requires, never from the type label. It decodes the allocation tables and the
directory tree, with file content digests, within fixed bounds that exceed the
run's resource limits. A scope the model did not decode completely is
**unclassified**. A bound that is reached leaves unclassified the scopes that
read what it cut off: a bound on the directory tree leaves `directories`,
`files`, `verification` and `destination`; a bound on the allocation tables
leaves `files` and `verification`; a bound on content digests leaves `files`. A
predicate whose violation leaves the meaning of a structure open, as a
`safety_only` rule's can, marks the scopes that read it as not decoded
completely.

Rules, domains and generators are Ruby procs and data. There is no rule DSL,
expression language, YAML or JSON rule format, `eval` or compiled binary-format
description; only rule metadata and case inputs are serialized. A rule is a
plain Ruby value with these fields:

| Field | Meaning |
| --- | --- |
| `id` | Stable identifier such as `fat.bpb.bytes_per_sector`. It is the regression key and never changes meaning. |
| `applies_to` | Predicate over the model and the run environment: variant, feature flags and preconditions |
| `predicate` | Pure function over the same inputs, plus the settings and tree for a creation rule. It returns the rule's violations and never runs fsimg: a rule with one scope answers whether it holds, and a rule whose scope varies per violation returns the scope of each violation. |
| `domain` | For a field, its legal values, explicit invalid examples and boundaries. For a relational rule, a generator of parameter overrides and mutations. |
| `category` | `normative`, `profile` or `advisory` |
| `policy` | What the stages that check a violation must do |
| `scope` | The check that must find a violation. It is fixed, or the predicate reports it per violation; see [stage policy](#stage-policy). |
| `status` | The [verdict status](#outcome-classes) a rejection reports, with the resource for `FSIMG_ELIMIT` |
| `source` | Specification document, structure, field and section, and the design section that places the check. Compatibility notes and observed tool behaviour are recorded apart from normative text. |
| `related` | Optional rules that a violation of this one necessarily affects; they explain multi-rule results |

The **category** separates the published format from fsimg's choices.
`normative` states the format. `profile` states fsimg's supported subset, its
strict verification policy, or a limit of the run. `advisory` covers hints and
recommendations. A requirement with a normative and a profile part is two
rules. For example, the specification allows any nonzero FAT count
(`fat.bpb.num_fats`, normative), while fsimg reads at most two
(`fat.bpb.num_fats.supported`, profile). Profile rules are reported separately,
so fsimg's policy never rewrites the specification. A noncanonical, recoverable
or advisory divergence is never declared invalid merely because one
implementation dislikes it.

The **policy** says what the stages that check a violation must do:

| Policy | Category | Stages that check a violation must | Example |
| --- | --- | --- | --- |
| `must_reject` | normative, profile | reject with `status` | Zero bytes per sector; a stale FSInfo free count at `verify` |
| `optional_feature` | profile | reject with `FSIMG_EUNSUPPORTED` | More than two FATs |
| `safety_only` | normative | reject with any verdict status, or accept with records that match the model | A QNX6 bootless volume's second-header position, which the [format design](design-qnx6.md#classic-volume-geometry) leaves to that profile |
| `must_accept` | advisory | accept, with records that match the model | Clusters larger than 32 KiB; an unknown FSInfo hint |
| `diagnostic_only` | advisory | as for `safety_only`; which of the two the stage did is listed as a diagnostic | A field no stage checks |

The registry refuses any other pairing of category and policy, apart from the
`normative` rules with policy `equivalent_contents`, defined under
[cases](#cases). When fsimg gains support for a feature, only the profile rule
that excluded it changes or disappears, and every generated expectation
follows.

### Stage policy

A stage is one fsimg invocation on a case. The reading stages are `info`, `ls`,
`extract`, `verify` and `verify-deep`; `create` is the writing stage. A
violation's scope names the check that must find it, and the scope fixes the
stages that check it:

| Scope | Check | Stages |
| --- | --- | --- |
| `open` | Probe and header-only open | `info`, `ls`, `extract`, `verify`, `verify-deep` |
| `directories` | Recursive directory traversal | `ls`, `extract`, `verify`, `verify-deep` |
| `files` | Reading file content | `extract`, `verify`, `verify-deep` |
| `verification` | Full structural verification | `verify`, `verify-deep` |
| `destination` | Creating host names and files | `extract` |
| `input` | Creation settings, source names and layout planning | `create` |

Each format's reader and writer designs own which checks run in which scope;
for FAT these are the [FAT reader](design-fat-reader.md) and the [FAT
writer](design-fat-writer.md). Every rule's `source` cites the section that
places its check. A check fsimg makes in one place, such as a BPB field checked
at open, has a fixed scope. A check fsimg applies wherever it reads, such as
chain validity, mirror agreement or chain sharing, takes its scope from where
each violation sits, and the predicate reports it. For example, the FAT reader
claims directory clusters during [directory
preparation](design-fat-reader.md#directory-preparation), but finds overlapping
file chains only in the ownership sort of [structural
verification](design-fat-reader.md#structural-verification-and-lifecycle). A
crosslink between two file chains therefore has scope `verification`, and
`extract` accepts it.

`info` performs both the format probe and the header-only open, so the harness
has no separate probe stage. Rule statuses follow the [status
contract](architecture.md#specification-driven-testing).

`verify` is strict, and the content stages are tolerant. Divergence that does
not affect content, including the reserved table entries, has scope
`verification`, where the FAT reader places it. Divergence the specification
allows, such as a stale FSInfo free count, is a `profile` rule. A `FAT[0]`
media byte that differs from `BPB_Media` breaks a normative requirement, so its
rule is `normative`.

Rules also read the **run environment**: the runner's [limits](#runner) and the
host's answers to the [name trials](#host-names). An image whose declared tree
exceeds a limit violates a `profile` rule with status `FSIMG_ELIMIT` and that
resource. Because the decoder's bounds exceed the limits, a tree one past a
limit is still classified, and the n-1, n and n+1 witnesses at a limit rest on
the counting definitions of the [bounded
resources](architecture.md#bounded-resources) contract. A name the host refuses
violates a rule with scope `destination`, which therefore depends on the host.

### Expectation derivation

For each stage of a case, let V be the case's violations whose scope the stage
checks.

1. If V contains a `must_reject` or `optional_feature` violation, the stage
   must reject. The allowed statuses are those of its `must_reject`
   violations, plus `FSIMG_EUNSUPPORTED` for an `optional_feature` one; if V
   also contains a `safety_only` or `diagnostic_only` violation, which fsimg
   may meet first, every verdict status is allowed.
2. Otherwise, if V contains a `safety_only` or `diagnostic_only` violation, the
   stage may reject with any verdict status, or accept with records that match
   the model.
3. Otherwise the stage must accept, and its records must match the model:
   `info` reports its variant, `ls` its paths, types and sizes, `extract` its
   tree with equal content digests, and `verify` its file, directory and
   logical-byte counts. An accepting `create` must produce an image that passes
   the [creation-case](#cases) checks.

A stage that checks a scope the model could not decode completely gets no
derived expectation. The case is unclassified at that stage: only safety is
asserted, and the report counts it separately. The harness deliberately does
not encode fsimg's internal order of checks. Under every policy, a stage must
also end `accepted` or `rejected` and pass the [safety checks](#runner).

## Cases

A case is one image and the stages run on it. Each case belongs to one class:

* **A. Valid.** A consistent geometry and tree are generated and built. The
  case must classify with no violation, or the generator is wrong. Every stage
  accepts, with records that match the model.
* **B. Targeted.** One parameter or relationship of a valid case is changed, by
  a rebuild with one parameter out of domain or by a mutation of a [formatter
  base](#fixtures), and the whole registry is re-evaluated. The intended rule
  must be among the violations, or the generator is wrong. If it is the only
  violation, the case is `single_rule`; otherwise it is `multi_rule`, and the
  recipe records the actual set. Mutating the sector size of a formatter base
  honestly yields a multi-rule case; the rebuild isolates the same rule.
  Targeted cases explain their own failures, unlike byte fuzzing.
* **C. General malformed.** Combined mutations, truncations and arbitrary byte
  edits. Classification is recorded, but the model may not see everything a
  random edit breaks, so only safety is asserted: every stage is
  `unclassified`. Acceptance or rejection is never counted as evidence of
  compliance.

A rule with policy `equivalent_contents` is `normative` and states a freedom of
the format rather than a requirement, so it has no violations. Its domain
transforms a valid case: it relocates a chain, reorders independent directory
slots, changes boot code, or sets the reserved high bits of FAT32 entries. The
transformed case must classify as its source does and decode to the same tree,
or the transformation is wrong. fsimg must then produce the same records for
both.

A **creation case** is a file tree and a set of creation settings, and `create`
always runs with `-R`. The harness writes the tree into `source/` through the
[name funnel](#host-names) and classifies the tree it walks back, not the
recipe. Rules with scope `input` state the domain of settings
and source trees and the writer's layout policy. For example, automatic sizing
keeps at least 16 clusters away from the FAT variant thresholds while `-s`
permits the exact boundary counts, and the writer refuses a source name that is
not a valid FAT name or that is case-equivalent to a sibling. The image an
accepted creation produces is classified like any other and must violate no
rule, including a writer `profile` rule that every byte outside allocated
metadata and file content is zero. Independent readers must reproduce the tree,
and a second creation from the same tree, written to disk in a different order,
must produce identical bytes. fsimg's reading stages then run on the created
image as secondary evidence.

### Recipes

A case is serialized as a versioned JSON recipe. The example below is
`single_rule` because the independent builder renders the 1000-byte sector size
coherently; the same field patched into a formatter base would also violate
`fat.geometry.within_image`.

```json
{
  "schema_version": 1,
  "id": "fat16.bpb.bytes_per_sector.invalid.1000",
  "format": "fat16",
  "base": {"builder": "independent", "tree": [],
           "parameters": {"bytes_per_sector": 1000, "sectors_per_cluster": 4,
                          "reserved_sectors": 1, "fat_count": 2,
                          "root_entries": 500, "clusters": 4085,
                          "media": 248}},
  "base_sha256": "<64 hexadecimal digits recorded at materialization>",
  "tool_versions": {"independent": "<digest of the builder source>"},
  "mutations": [],
  "stages": ["info", "ls", "extract", "verify", "verify-deep"],
  "kind": "targeted",
  "intended_violation": "fat.bpb.bytes_per_sector",
  "actual_violations": [{"rule": "fat.bpb.bytes_per_sector", "scope": "open"}],
  "isolation": "single_rule",
  "expected": {"info": "reject:FSIMG_EFORMAT", "ls": "reject:FSIMG_EFORMAT",
               "extract": "reject:FSIMG_EFORMAT",
               "verify": "reject:FSIMG_EFORMAT",
               "verify-deep": "reject:FSIMG_EFORMAT"}
}
```

`id` is stable, and `format` is the fsimg format name. `stages` lists every
stage the case can run; a profile runs the subset it selects, and a [promoted
case](#reports-and-regressions) runs all of them. A reading case carries
`base`, `base_sha256`, `tool_versions` and `mutations`, plus an optional
`forced_format` passed to every reading stage; a creation case carries a
`source` object holding the `tree` and the `settings` instead. A tree is a list
of entries, each with a relative `path`, a `type` of `file` or `directory`, a
`size` and a `seed`. The harness generates each file's bytes from its seed and
offset with its own function, never with PropCheck's random source, so a tree
stays stable across upgrades. `kind` names the case class: `valid`, `targeted`
or `malformed`. `expected` maps each stage to its allowed outcomes separated by
`|`: `accept`, `reject:STATUS` for each allowed status, or `reject:*` for any
verdict status; `unclassified` alone marks a stage that checks a scope the
model could not decode completely. An `FSIMG_ELIMIT` status carries its
resource, as in `reject:FSIMG_ELIMIT:entries`. The optional fields are
`random_seed`, `source_revision` and `resource_limits`; a promoted case also
carries a `reason`.

`intended_violation` (named by a targeted case and null otherwise),
`actual_violations`, `isolation` and `expected` record the classification and
derivation made when the recipe was written. They are never inputs: replay
re-derives them from the current rules and compares.

Recipes are untrusted input, arriving in promoted cases, replay requests and CI
artifacts, including those of pull requests. The loader parses them with
`JSON.parse`, never `JSON.load`. Before reading any field it refuses a recipe
text larger than 64 MiB, any string or object key that is not valid UTF-8 or is
longer than 1 MiB, and any number that is not an integer of at most 64 bits, so
every later check reads well-formed text and bounded numbers. It refuses an
unknown schema version or mutation, a missing required field and an unknown
one. It also refuses an `id` longer than 128 bytes, outside `[a-z0-9._-]` or
starting with `.` or `-`; a `format` or `forced_format` the harness does not
model; a `base_sha256` that is not 64 hexadecimal digits; a parameter or
setting that is not an integer; a parameter or setting name that is not
`[a-z][a-z0-9_]*` of at most 64 bytes, or a builder or tool name that is not
`[a-z][a-z0-9._-]*` of at most 64 bytes; more than 256 parameters, settings or
tools; an unknown tree entry type; a tree path that is empty, is absolute, or
contains `..`, `.`, an empty component or NUL, that repeats, or whose parent is
not a directory entry of the same tree; a directory entry with a size or seed;
an unknown `kind`, or an `intended_violation` named by a case that is not
targeted or missing from one that is; an `expected` that does not name exactly
the `stages`; a reading case that runs `create` or a creation case that does
not; more than 4096 mutations; a promoted case without a `reason`; and any
size, parameter, mutation or `resource_limits` value beyond the profile's
ceilings. Refusal messages cut the values they quote short. Recipe values check
the format when they are built, so a recipe a generator builds in code obeys it
too; only the ceilings are the loader's. Because every string is UTF-8, source
names that are not UTF-8 are outside what a recipe can express. The canonical
form has a fixed field order, sorted map keys apart from `expected`, which
follows the order of `stages`, and two-space indentation; the
harness writes it with its own serializer, because the json gem's pretty
printer varies across versions and recipe bytes key the fixture cache. The
loader accepts only text in canonical form, which also refuses duplicate keys
that `JSON.parse` would resolve silently, so a loaded recipe round-trips to
identical bytes. A random seed alone does not reproduce a case across PropCheck
or formatter upgrades, so recipes carry explicit parameters, mutations and
trees.

### Mutations

| Mutation | Effect |
| --- | --- |
| `set_u8(offset, value)` | Writes one byte |
| `set_u16le(offset, value)`, `set_u32le(offset, value)` | Writes a little-endian integer |
| `replace_bytes(offset, bytes)` | Overwrites a byte range, given in hexadecimal |
| `truncate(length)` | Shortens the image |
| `set_fat_entry(table, cluster, value)` | Writes one allocation-table entry, including FAT12 packing |
| `set_directory_entry(directory, index, fields)` | Rewrites named fields of one directory slot |
| `splice_chain(file, next_cluster)` | Redirects a file's chain |
| `relocate_chain(file, clusters)` | Moves a file's clusters and rewrites its chain |
| `reseal(structure)` | Recomputes a named integrity field from the bytes it covers: an exFAT boot or entry-set checksum, the QNX6 superblock CRC, or a FAT long-name checksum |

Every mutation checks its bounds and never wraps. It refuses an offset or value
that does not fit its target, and a truncation that would lengthen the image.
No mutation consults the rules, so any invalid encoding can be written.
Semantic mutations address structures by table, cluster and path rather than by
byte offset, so they survive a base that re-materializes with different bytes.
`reseal` keeps the image's integrity intact after a field mutation, so the
field's own rule is the only one violated.

## Fixtures

A **base** is a recipe, never a committed image: formatter output differs
between tool versions, and a committed fixture would freeze one version's
bytes. The image is materialized once into the build directory's cache under
its **cache key**, a digest of the canonical base recipe and the versions of
the tools that build it; the independent builder's version is a digest of its
own source. A cache entry is written under a temporary name and published with
a no-replace link, so a concurrent run never reads a partial image or replaces
one in use.

After materialization the harness refuses a base whose decoded variant or
geometry differs from its parameters, so a formatter that silently substitutes
another geometry never supplies a witness. It then classifies the base like any
case. A base meant to be valid must classify valid, decode to its recipe's
tree, and pass the read-only checkers that support its geometry; otherwise it
is reported and serves no case. Every base has a manifest in the
[fixture-manifest shape](design-formats.md#format-descriptors-and-profiles):
format, geometry and feature profile; builder, recipe and tree digest; the
creation command; validator results; and the image hash per tool version.

Bases come from two kinds of builder. **Formatters** are `mkfs.fat --invariant`
with a fixed volume ID on macOS and Linux, or `newfs_msdos` on FreeBSD,
populated with `mmd` and `mcopy -m` from source files with fixed times, which
the [name funnel](#host-names) writes into `test/spec-cache/<key>.src/` like a
creation case's source tree and which is removed once the base is built; their
output is a **formatter base**. A typical command is
`mkfs.fat -C --invariant -i 12345678 -F 32 -s 1 base.img 65536`, with the size
in 1024-byte blocks. The manifest records the read-only checker
(`fsck.fat -n` or `fsck_msdosfs -n`) and an `mdir` listing. Formatter bases
test both the model and fsimg against images neither produced.

**The independent builder**, one per format, renders an image from explicit
parameters: for FAT, the boot sector and BPB, any number of tables with their
reserved entries, the FAT12/16 root region or FAT32 root cluster, FSInfo, the
backup boot sector and a small file tree. It computes every dependent field and
integrity value from its parameters and never validates them, so it renders
out-of-domain values coherently. It exists for geometries formatters will not
produce (exact threshold cluster counts, one FAT or more than two, 4096-byte
sectors at the thresholds, clusters above 32 KiB), for targeted witnesses of
fields that other fields depend on, and for hosts without formatters.

A valid image of another geometry is always rebuilt, never patched: a
512-byte-sector image with its sector size rewritten is not a valid
4096-byte-sector image. An absent formatter or checker is unavailable for that
tool only; the core tests still run on builder bases. A case image is written
only when a process reads it, as a sparse copy of the base's data regions found
with `SEEK_DATA` and `SEEK_HOLE`, so a 64 MiB FAT32 base never costs its
logical size per case.

Replay rebuilds a case's base from its recipe. If the image hash matches the
recorded one, the replay is exact. Otherwise, because a tool version differs or
a tool stamps the current time, the harness re-classifies the case. The replay
is equivalent if the recorded violations and expectation still hold, and stale
if they do not. A base's identity for replay is therefore its classification,
and a nondeterministic tool needs no special handling.

## Independence and oracles

Rules, layouts, the model and the builders are written from the published
format documents, never ported from `lib/formats/` or from the C tests. Only
that keeps them independent evidence, and reviews check it. Each layout is
asserted against an offset table transcribed by hand from the specification.
GPL tools such as dosfstools and mtools run only as executables, and nothing is
copied from them.

The model and the builder are checked against each other through independent
tools: the model decodes formatter output, and checkers read builder output. A
builder base and a formatter base of the same geometry must decode to equal
models. Beside each case's derived expectation, oracles are secondary evidence:
mtools reads the tree of a valid image; for an image fsimg created, `mcopy -s`
must reproduce the tree, with `fsck -n` and fatcat as further evidence; for an
invalid image, checker diagnostics are only recorded.

An oracle is evidence, not a verdict. A checker can flag conservative
incompatibilities or tolerate errors, so its result never overrides a published
rule; where the specification is clear, it decides. Disagreements are recorded
per tool, never resolved by majority, and expected at the exact variant
thresholds, which some implementations avoid. `fsimg create` followed by
fsimg's own reading stages is never evidence on its own, because both paths can
share a defect.

## Runner

`runner.rb` is the only code that knows fsimg's command syntax. fsimg runs as a
[coordinator](design-cli.md#coordinator-and-streaming-representation) process
that supervises its workers. Every command runs with the case directory as its
working directory and relative operands, so fsimg is given no host path to
report. Sanitizer reports and Ruby backtraces can still carry build, gem and
home paths.

| Stage | Command |
| --- | --- |
| `info` | `fsimg info -j -i image.img` |
| `ls` | `fsimg ls -j -r -i image.img` |
| `extract` | `fsimg extract -j -i image.img -C dest` |
| `verify`, `verify-deep` | `fsimg verify -j [-d] -i image.img` |
| `create` | `fsimg create -j -R -t FORMAT [-s BYTES] [-O sector=N] [-O cluster=N] -o out/created.img source` |

A reading stage adds `-t FORMAT` when the recipe forces a format. Every
invocation passes the profile's ceilings: `-m 64M`, `-l` values for `seconds`,
`entries`, `depth`, `path`, `output`, `scratch` and `fds`, and `-T scratch`.
Extraction holds one descriptor per directory level, so `fds` is the larger of
fsimg's default and the depth ceiling plus a fixed margin. No `work` limit is
passed, because fsimg's own algorithmic bounds are under test and the deadline
catches an unbounded loop. The sandbox policy stays at the default, `auto`;
`FSIMG_SPEC_SANDBOX=required`, which CI sets on Linux and FreeBSD, passes
`-S required`. The runner never passes `-S off`.

fsimg sees a fixed environment and nothing else: `PATH` from the wrapper,
`LC_ALL=C`, `TZ=UTC`, `HOME` set to the case directory, `TMPDIR` set to its
scratch directory, and `ASAN_OPTIONS`, `UBSAN_OPTIONS`, `LSAN_OPTIONS` and
`MSAN_OPTIONS`. The runner sets all four even when the build has no sanitizer,
to any value the sanitizer tree passed followed by
`abort_on_error=0:log_path=stderr:exitcode=87`. A later key overrides an
earlier one, so a sanitizer report in the coordinator always reaches stderr and
ends it with status 87. Without these keys, Apple platforms default
`abort_on_error` to true, which aborts instead and leaves a crash report under
`~/Library/Logs`, and MSan's own default status, 77, reads as a skip to
automake. fsimg's workers receive fixed sanitizer options of their own from
`compat/spawn.c`.

Oracles see the same `PATH` and `TZ`, a UTF-8 locale (`C.UTF-8`, or
`en_US.UTF-8` where the host lacks it) so that mtools keeps Unicode long names,
`HOME` set to `oracle-home/`, which they cannot write, and `MTOOLSRC` naming
the empty file in it. Nothing fsimg writes into a case directory can therefore
configure an oracle. mtools still reads a system `/etc/mtools.conf`; the
harness accepts it as part of the host's mtools, and names images only with
`-i`, so the file cannot redirect a drive to a device. Every argument built
from a name starts with a fixed prefix, such as `source/` or `::/`, so a
leading `-` is never read as an option.

Each profile's results directory holds only these names, and every case and
failure directory in it is created exclusively; a second case with the same id
in one run is a [`harness_error`](#outcome-classes):

```text
test/spec-results/<profile>/
  coverage.json, summary.txt   the report
  cases/<id>/                  case directories
  failures/<id>/               failure records
  trials/                      name trials
  preflight/                   the preflight's case directory
  oracle-home/                 the oracles' HOME, with an empty mtoolsrc
```

Before any case, a **preflight** establishes the host in `preflight/`, which it
removes when it finishes. A valid builder base runs through every reading
stage, and one valid creation runs, with the run's options; any failure stops
the run with `harness_error`. Every later invocation must report the
preflight's sandbox status. The preflight also checks:

* that the longest tree root the runner walks by path, a case's `dest/` or
  `source/` with a 128-byte id or a population tree
  `test/spec-cache/<key>.src/`, plus the profile's `path` ceiling, stays below
  the host's `PATH_MAX`, because the runner walks by path while fsimg works
  descriptor-relative;
* that the soft `RLIMIT_NOFILE` fsimg inherits is at least the `fds` limit;
* that `trials/` and `cases/` report the same `st_dev`;
* when oracle confinement is configured, that a fixed command run through it
  can neither write a file outside the case directory nor read a sentinel file
  the harness placed outside it.

Each invocation follows one lifetime:

1. The runner spawns fsimg in a new process group under the limits below.
2. It reads stdout and stderr through pipes, each up to its cap, while it waits
   for the coordinator by pid with `Process.wait(pid)`. It never uses
   `wait(-1)`, `Open3` or `Process.detach`, which reap in helper threads.
3. If the wall-clock limit or a cap is reached first, it kills the group and
   then reaps the coordinator.
4. Once the coordinator is reaped, it probes the group with
   `Process.kill(0, -pgid)`. Success means a process outlived the
   coordinator: the runner records it as a leftover and kills the group. A
   group with members keeps its ID allocated, so the kill cannot reach a
   reused ID. The one residual race is that the last member exits, and a new
   group leader takes the ID, between the probe and the kill.
5. It reads both pipes to their end and keeps what it read in `STAGE.stdout`
   and `STAGE.stderr`.

`SIGINT` or `SIGTERM` to the harness kills every live case group. A process
that leaves its group with `setsid` escapes these steps; fsimg never does so.
Oracles follow the same lifetime and limits, each with its own deadline. The
limits are:

* a wall clock of fsimg's own deadline plus a grace period longer than the
  coordinator's shutdown deadlines, so fsimg's deadline normally fires first
  and reports itself;
* `RLIMIT_CPU` slightly above the wall limit, which bounds a spinning process;
* `RLIMIT_CORE` of zero;
* `RLIMIT_FSIZE` at the largest of the image, extracted-byte and scratch
  ceilings, so a legitimate `FSIMG_ELIMIT` never becomes `SIGXFSZ`;
* `RLIMIT_AS` of 1 GiB where the host enforces it (Linux and FreeBSD) and
  `fsimg -V` starts under it, which a sanitizer build's address reservation
  prevents; the result records whether it applied;
* a capture cap of the coordinator's [text
  ceiling](architecture.md#bounded-resources) for the limits passed, plus a
  fixed allowance for text the coordinator does not count, such as a sanitizer
  report. The profile's `entries` and `path` limits keep it within tens of MiB,
  and reaching it means fsimg exceeded its own output bound.

Each case runs in a fresh directory:

```text
cases/<id>/      working directory and fsimg's HOME; the only place tools write
  image.img      the case image; no reading stage writes it
  scratch/       -T and TMPDIR
  dest/          the extract destination; absent before extract
  source/        the creation input tree
  out/           created.img and its staging
  oracle/        files that independent tools write
  STAGE.stdout   bounded captures, one pair per stage
  STAGE.stderr
```

After every stage the runner checks the following:

* the image's size and modification time are unchanged;
* the case directory holds only the names above;
* `dest/`, walked with `lstat` without following links, holds only directories
  and regular files, whose total logical size stays within the `output` limit.
  Each entry records its path, type, size and SHA-256;
* `scratch/` is empty and `out/` holds nothing beyond `created.img`, except a
  path the error record names in `retained_scratch` or `retained_output`;
* no leftover was recorded in step 4.

A failed check fails the case under every policy and ends it: no further stage
or oracle runs on it. The runner gives fsimg and every oracle only paths inside
the case directory, and refuses an image path that is not a regular file there,
so no tool reads a host device. Writes outside the case directory are beyond
these checks; the sandbox contains them where the host provides one. Each case
owns its directory and reads the cache only through published entries, so cases
are independent and a run may execute them in parallel. A passing case's
directory is deleted after its last stage.

### Host names

Every name the harness takes from a model or a recipe reaches the host through
one **name funnel**. Names are binary strings (`String#b`) throughout: the
funnel builds each path by concatenating binary strings under a binary
directory path, never with `File.join`, because Ruby otherwise transcodes or
rejects a name by its encoding tag. It creates one component at a time, in a
directory it created itself, with exclusive creation and without following
links. It refuses, and never creates, a component that is empty, `.` or `..`,
or contains `/` or NUL; the model already reports such a name as a violation.
It reads only four answers: success, `EEXIST`, `ENAMETOOLONG` and `EILSEQ`; any
other error is a `harness_error`. It lists a directory back with
`Dir.each_child` and `File.lstat`, never `Dir.glob`, which skips names that
start with a dot, and compares bytes.

For an extract case, a **name trial** puts each sibling set of the model's tree
through the funnel, as empty files in a fresh flat directory of its own, and
lists it back. A sibling answered `EEXIST` collides with an earlier one and
violates a `destination` rule with status `FSIMG_EEXISTS`; one answered
`ENAMETOOLONG` or `EILSEQ` violates a `destination` rule with status
`FSIMG_EUNSUPPORTED`. No trial path nests, so path traversal and path length
cannot arise. Answers are cached for the run by the sibling set's bytes, so a
repeated set costs no trial. For a creation case, and for the files that
populate a formatter base, the funnel writes the source tree and the harness
walks it back; the case is classified against the walked tree.

If a listing or walk differs from the names the funnel created, because the
host refused a name or stored another one (HFS+ normalization, msdosfs trailing
dots), the host cannot hold the case's tree. The case is reported as a **host
skip**, and fsimg does not run.

Destination violations depend on the host: an NFC/NFD pair collides on APFS but
not on ext4. A recipe therefore never records them, the intended-rule and
promoted-equality checks ignore them, and each stage result records the trial
answers instead. A destination witness that gets no violation on a host runs
there as an ordinary case, and its rule counts as not exercised on that host.

Trials run in `trials/`, never in a case directory. `trials/` and `cases/` are
created fresh in one results directory, so they share its filesystem and the
per-directory name attributes it passes on, such as ext4 casefold. The runner
removes each trial directory when the trial ends, and all of them when a run
stops, including on `SIGINT` or `SIGTERM`.

### Outcome classes

A rejection is read from the integer `code` of the CLI's `-j` error record,
under the [status contract](architecture.md#specification-driven-testing);
neither the exit status nor the prose `status` string decides it.

A **verdict status** is one that image content, creation input or the
destination can cause when fsimg and the host work correctly. The preflight
establishes the host capabilities that could otherwise produce these statuses,
and name trials give the host's own answers for the destination's names, so a
verdict status reflects the case. Any other code reports a fault. The registry
refuses a rule that names a status other than a verdict status of the stages
that check it.

| Stages | Verdict statuses |
| --- | --- |
| Reading stages | `FSIMG_EFORMAT`, `FSIMG_ECORRUPT`, `FSIMG_EUNSUPPORTED`, `FSIMG_ERANGE`, `FSIMG_ELIMIT` |
| `extract`, additionally | `FSIMG_EEXISTS`, for a destination name collision |
| `create` | `FSIMG_EINVAL` for refused settings or source names, `FSIMG_ERANGE` for source content or layout beyond a format field, `FSIMG_EUNSUPPORTED`, `FSIMG_ELIMIT` |

| Outcome | Condition |
| --- | --- |
| `accepted` | Exit 0, and the last record is a `summary` with `complete:true` |
| `rejected(STATUS)` | Exit 1. The last record is the invocation's only `error` record, its code is a verdict status of the stage, and for `FSIMG_ELIMIT` its `resource` field names the resource. Exit 2 counts only at `create`, when a violated `input` rule names `FSIMG_EINVAL`. |
| `faulted(STATUS)` | As `rejected`, but the code is not a verdict status, such as the `FSIMG_EIO` of an I/O, spawn or worker-protocol failure or the `FSIMG_ESTATE` of an internal-state failure. The parser worker's standard streams are `/dev/null`, so its sanitizer reports arrive as `FSIMG_ESTATE`. |
| `crashed` | The coordinator ended by a signal the runner did not send |
| `timed_out` | The runner's wall-clock limit expired, or fsimg reported its own deadline as `FSIMG_ECANCELLED` |
| `output_exceeded` | A capture reached its cap |
| `sanitizer_failure` | The coordinator exited with status 87, or stderr contains `ERROR: AddressSanitizer`, `ERROR: LeakSanitizer`, `WARNING: MemorySanitizer` or `runtime error:` |
| `harness_error` | Anything else: a launch failure, any other exit 2, exit 0 without a completion summary, exit 1 without exactly one error record, malformed NDJSON, a sandbox status that differs from the preflight's, or any other exit status |

When several conditions hold, the first match decides, in this order: a launch
failure, a sanitizer report, the runner's wall-clock limit or a capture cap, a
signal, then the records. Only `accepted` and `rejected` can satisfy an
expectation; every other outcome fails the case under every policy. Each stage
result records its outcome, exit status and any signal, its wall and CPU time,
the bounded captures and parsed records, whether the address-space limit was
applied, the extracted tree digest, the name-trial answers and the oracle
observations.

### Process boundary and host safety

The CLI is the only adapter. It exercises the worker, its protocol and the
sandbox, and a subprocess contains whatever a corrupt image does to the parser.
There is no in-process driver over the C API, Ruby FFI is not used, and fsimg
gains no command, option or interface for testing alone.

Generated images are never mounted. A hostile image can attack an oracle as
well as fsimg, so oracle exposure follows an image's provenance, not its
classification. Oracles read class A bases and images fsimg created in every
profile. Other images reach them only in the nightly and deep profiles, and
only through an OS sandbox command named by `FSIMG_SPEC_ORACLE_CONFINE`, as the
preflight checks. The sandbox allows writes only in the case directory, and
reads only there and in the tool's installation and shared libraries, locale
data, `/etc/mtools.conf` and `oracle-home/`, the same files an unconfined
oracle reads. Without one, those observations are reported unavailable.

## Generation and shrinking

Deterministic cases are mandatory in every profile. The generators enumerate
every small legal domain, every listed invalid example, every documented
sentinel, and the neighbours n-1, n and n+1 of every boundary. For FAT these
include the 4084/4085 and 65524/65525 cluster counts, table capacity at an
exact fit and one entry short, and the allocation-table markers. They never
depend on what random draws produce.

Generation is constructive. A generator chooses the independent parameters and
computes the dependent fields in order. For FAT the independent parameters are
sector size, cluster size, reserved count, table count, root entries and
cluster count. A targeted witness replaces one parameter with an out-of-domain
value and recomputes the rest from it.

PropCheck explores beyond the enumerated points. It generates recipes, not raw
images, so shrinking yields meaningful minimal cases. Random invalid draws need
no filter, because classification derives their expectation. Random draws
mutate shared bases and never invoke a formatter per draw; they are classified
in memory, apart from name trials for sibling sets not yet tried in the run, and
only a sample sized by the profile runs fsimg.

Shrinking starts with PropCheck's shrinker on recipes. Recipe shrinkers then
remove unrelated files and directories, shrink files and chains, reduce a cycle
to two clusters and a crosslink to two files sharing one cluster, drop unneeded
mutations, and move values toward the nearest boundary. A shrink step is kept
only when the case still fails the same way and its classification is
unchanged, so shrinking never turns a witness valid or changes the rule it
tests, and the check costs no fsimg invocation.

### Constraint solving

The harness has no constraint solver. Each recurring constraint family has a
generation order in which every quantity follows from ones already chosen:

| Family | Constructive answer |
| --- | --- |
| FAT cluster counts at the variant thresholds with tight table capacity, across sector size, cluster size, reserved count and table count | Choose the cluster count first. The table size follows in closed form, including 12-bit packing, and the total sector count follows from that. |
| Alignment and placement: the data region, FSInfo and the backup boot sector, the exFAT table and heap offsets | Compute padding after the quantities it aligns |
| A field whose change cascades into dependent geometry | Rebuild with the one out-of-domain value. Where it leaves a dependent undefined, the dependent rules do not apply. |
| Integrity fields: exFAT checksums, the QNX6 superblock CRC, FAT long-name checksums | Compute each after the bytes it covers, or `reseal` after a mutation |
| Allocation state: the exFAT and QNX6 bitmaps and free counts, QNX6 header framing and tree depths | Allocate first, then derive; framing is closed form from the slice length and block size |

A targeted witness is reported impossible only when exhaustive in-memory
enumeration of the generator's bounded parameter space finds none. A solver
would add a native dependency, version-dependent witnesses and a second
encoding of every rule. It is reconsidered only when the coverage report shows
targeted witnesses neither generated nor shown impossible in at least three
rule families, and would then stay optional and outside CI.

## Profiles

| Profile | Entry point | Contents | Target |
| --- | --- | --- | --- |
| `quick` | `make check`, `make check-spec` | Harness self-tests; every deterministic witness through fsimg at the first stage that checks its violation, and valid witnesses through every stage; 100-300 PropCheck examples, of which about 30 per variant run fsimg; every promoted case | At most about 60 s added to `make check` on the macOS host. Beyond that, `make check` keeps a smoke subset and `check-spec` runs the rest. |
| `nightly` | `make check-spec-extended` | Thousands of random examples, deterministic witnesses through every stage, 50-150 oracle cross-checks, class C cases | 10-30 min |
| `deep` | `make check-spec-deep` | Large volumes, broad mutation matrices, every oracle on every case | Explicit opt-in |

The budgets are targets, which measurement adjusts. Every profile sets ceilings
on image capacity, file count, path depth, memory, scratch, extracted bytes and
per-case run time; `profile.rb` holds their values. The runner passes them to
fsimg as `-m` and `-l` values, and the generators and the recipe loader keep
within them. The `path` ceiling leaves room under the 1024-byte `PATH_MAX` of
macOS for the case directory's own path. Fuzzing stays in
`check-fuzz`, which `bin/export-seeds` supplies with the deterministic invalid
witnesses and the promoted cases as seeds; the native fuzzers never call Ruby.

## Reports and regressions

Each run writes `coverage.json` and `summary.txt` into its profile's results
directory, and `test/spec.sh` prints the summary at the end. Both name the
profile, the Ruby version and the Minitest seed, and count the run's Minitest
outcomes. A run holds an
exclusive lock on that directory, so concurrent runs of one profile take turns,
and empties it when it starts, so no case, trial or failure directory outlives
the next run. For each rule the report records the attempted, passed, failed
and skipped counts, the boundaries exercised, the stages asserted, the oracle
results, the promoted regression cases and the isolation: `single_rule` when
every targeted witness isolates the rule, `partial` when some do, and
`multi_rule_only` when none do.

It keeps these states distinct:

* rule declared but not exercised;
* valid witness available or unavailable;
* targeted witness available, impossible within the enumerated space, or not
  generated;
* behaviour asserted against fsimg, only the Ruby predicate tested, or
  unclassified;
* base accepted by its formatter, or a geometry the tool does not support;
* case run, or a host skip because the host cannot hold its tree;
* oracle agrees, disagrees, is not applicable or is unavailable;
* run confined by a sandbox or not.

A declared rule never counts as covered until fsimg has run its cases, and a
rule whose targeted witnesses are all multi-rule is reported as such; neither
declarations nor entangled witnesses make a coverage number. The report orders
entries by rule and case ID, so its content does not depend on execution order.
A Ruby exception while handling a case makes that case a `harness_error`; the
run continues and fails at the end.

Every final mismatch writes a failure record in `failures/<id>/` of the results
directory, created exclusively like a case directory: the recipe with its
classification and expectation (`case.json`), the base recipe, image hash and
tool versions (`fixture.json`), the bounded captures, the oracle observations,
the outcome, timings and safety checks of each stage (`result.json`), the image
when it is sparse and at most 64 MiB logical, and a `replay.sh` holding the
fixed line `bin/replay case.json`. `bin/replay` rebuilds and reruns one case,
re-deriving its classification and expectation from the current rules. CI
uploads only `coverage.json`, the summary and `failures/`.

A confirmed defect is promoted by copying its recipe into
`test/spec/cases/<format>/` with its `reason` and with `regression.` prefixed
to its id, so a promoted case and the witness it came from never share a case
or failure directory; no generated id starts with `regression.`. The promoted
recipe is the defect's regression test: it fails before the fix and passes
after it. It uses an independent-builder base wherever the defect reproduces on
one, so that it runs on every host; the quick profile replays every promoted
case whose builder the host has, and reports the others as skipped. A promoted
case's recorded violations and expectation must equal the re-derived ones, so a
rule change that alters a regression case's meaning fails until the same commit
updates the recipe.

Recipes are preferred to committed images. A mismatch is triaged as an fsimg
defect, a rule defect, a missing rule, or a policy question for this document.

## Placement and toolchain

The harness is Ruby under `test/spec/`, in the module `FsimgSpec`, because the
project's tests live in `test/`. It uses three development gems, locked with
Bundler at versions that run on Ruby 3.2 and later: Minitest for assertions,
PropCheck for generation and shrinking, and BinData for fixed record layouts.
Ruby 3.2 is the first release with `Data.define` and the one Ubuntu 24.04
ships. Minitest brings drb and prism, which only its own command-line runner
uses; the harness calls `Minitest.run` directly. The gems are not vendored and
never become runtime dependencies of the library or the CLI.

`make` owns every entry point, and there is no Rakefile. `test/spec.sh`,
generated by `configure`, runs `test/spec/run.rb --profile=NAME`. The quick
profile is a member of `TESTS`, so `make check` runs it, and so does every
sanitizer tree, because `check-asan`, `check-ubsan`, `check-lsan` and
`check-msan` run `make check` in their own builds; `make check-spec` runs it
alone. `make check-spec-extended` runs the nightly profile and is part of
`check-extended`, and `make check-spec-deep` runs the deep profile on request.

The wrapper exits 77 when Ruby 3.2 or the locked gems are absent, as
`test/cli.sh` does for its interpreter. CI installs them, so CI never skips the
suite. The [plan's build
section](../docs/spec-testing-plan.md#build-dependencies-and-ci) specifies the
wrapper's detection and environment.

Generated output stays in the build directory, so VPATH and `distcheck` builds
never write into the source tree. Base images go to `test/spec-cache/`; case
directories, reports and failure records go to `test/spec-results/<profile>/`.

```text
test/spec.sh.in             wrapper
test/spec/
  Gemfile, Gemfile.lock     locked gems
  run.rb                    runs one profile and writes its report
  lib/fsimg_spec/           format-independent core
    contract.rb             fsimg's stages, scopes, statuses and resources
    profile.rb              profiles, their ceilings and results directories
    results_directory.rb    the results lock and emptying
    rule.rb                 Rule, Domain, the registry and its checks
    expectation.rb          expectation derivation
    recipe.rb               recipes and the checks on their values
    recipe_loader.rb        the recipe loader
    canonical_json.rb       the canonical JSON form of recipes
    mutation.rb             mutations
    case.rb                 a recipe's classification and recorded fields
    image.rb                in-memory views and sparse materialization
    runner.rb               the only code that knows fsimg's syntax
    tree.rb                 tree model, lstat walk, digests, name funnel
    fixtures.rb             base recipes, cache, manifests, tool adapters
    report.rb               coverage and failure records
  lib/fsimg_spec/fat/       layout, model, geometry, rules, builder,
                            generators
  suite/core/               harness self-tests; fsimg is never run
  suite/fat/                FAT layout, rule and end-to-end tests
  cases/fat/                promoted regression recipes
  bin/                      replay, report, export-seeds
```

A shared abstraction is extracted only where a second format exposes real
duplication. A change to the core's rule semantics, recipes or report bumps the
corresponding schema version.
