# RFC: Specification-driven testing for fsimg (Ruby edition)

**Status:** Implementation proposal  
**Date:** 2026-10-19  
**Targets:** FAT12, FAT16, FAT32 first; exFAT, NTFS, QNX4/QNX6FS, HFS+, APFS and others later  
**Implementation language:** Ruby 3.2+ for the test/specification layer; existing C for fsimg and libFuzzer targets  
**Guiding principle:** Reuse working test tools, not build another test framework, SMT system, binary DSL, or filesystem implementation.

## Executive decision

Build a small Ruby test library using **Minitest + PropCheck + BinData**. Describe on-disk binary layouts in BinData (and use Ruby `pack`/`unpack` for small, fixed-offset mutations). Describe validity and behavior rules as plain Ruby objects and predicates. Build **valid images constructively** with a reference formatter plus a small set of known-good fixtures. Start **negative cases** from validated images and introduce named mutations. Verify behavior against the original logical file tree, external tools such as **mkfs.fat, mtools, fsck.fat**, and the specification. Keep the existing **C/libFuzzer + ASan/UBSan** testing loop, and contribute generated regression seeds to it.

**Do not add Z3 to version 1.** Add an optional, aggressively time-bounded Ruby Z3 adapter only when constructive generation of a particular numeric geometry becomes genuinely cumbersome. Never use it to solve complete filesystem images.

The distinctive output is **specification coverage**, not just code coverage: for each rule, track valid witnesses, invalid witnesses, boundaries, conditions exercised, oracle classifications, and stable reproduction recipes.

### Design goals

1. Change one field domain or constraint in the specification and automatically expand generated tests.
2. Generate both **valid** and **targetedly invalid** image cases with explicit expected behavior.
3. Test the complete pipeline: **probe → enumerate/list → extract**, and independently verify **creation**.
4. Catch arithmetic errors, malformed metadata handling, endless loops, unsafe paths, resource exhaustion, and cross-implementation incompatibilities.
5. Produce reproducible, small failure descriptions that can be promoted to permanent regression tests.
6. Reuse the same generic harness across filesystems without forcing different formats into an inflexible common schema.
7. Run a useful PR suite in minutes; keep nightly fuzzing, large-image tests, and external tools in separate, bounded jobs.

### Non-goals

- Formal proof of compliance or completeness.
- Exhaustively enumerate every 16/32/64-bit field combination or filesystem graph.
- Define a new YAML/JSON expression language or compile a new binary-format DSL.
- Reimplement a full FAT writer, filesystem driver, or general virtual block device in the first milestone.
- Depend on a solver in development or CI.
- Declare every noncanonical, recoverable, or advisory metadata mismatch invalid.
- Treat a single reference implementation or checker as the specification.
- Mount malformed images directly via the host kernel.

## 1. Reuse matrix and dependencies

| Need | Choice | Why |
| --- | --- | --- |
| Property-based generation/shrinking | [PropCheck](https://github.com/Qqwy/ruby-prop_check) | Ruby generators, composition, shrinking, Minitest integration; MIT license |
| Binary layout / serializer | [BinData](https://github.com/dmendel/bindata) | Declarative bidirectional binary records; BSD-2-Clause license |
| Deterministic tests / assertions | Ruby [Minitest](https://github.com/minitest/minitest) | Lightweight, familiar, no bespoke runner |
| Task orchestration | [Rake](https://github.com/ruby/rake) | Existing Ruby convention, straightforward CI tasks |
| File/recipe reports | Ruby stdlib `json`, `digest`, `fileutils`, `tmpdir`, `open3` | No dependencies necessary |
| FAT source formatter | [dosfstools `mkfs.fat`](https://github.com/dosfstools/dosfstools) | Create independently formatted FAT12/16/32 images |
| FAT independent I/O | [GNU mtools](https://www.gnu.org/software/mtools/) | List, copy, and extract without mounting |
| FAT diagnostic consistency check | `fsck.fat -n` in dosfstools | Read-only consistency evidence, not a normative verdict |
| Native/coverage-guided fuzzing | [LLVM libFuzzer](https://llvm.org/docs/LibFuzzer.html) | Preserve existing fast C fuzzing, ASan/UBSan |
| Optional numeric constraint solving | [Ruby `z3` gem](https://github.com/taw/z3) and Z3 native library | Only for small, hard-to-construct scalar models |
| Optional experimental Ruby-native fuzzing | [Ruzzy](https://github.com/trailofbits/ruzzy) | Not needed: adds complexity and license questions; avoid in v1 |

**Install policy:** Pin gems with Bundler and commit `Gemfile.lock`; record exact tool versions in CI. BinData 3.0.1 (BSD-2-Clause) and PropCheck 1.0.2 (MIT) are available as of this RFC. Third-party tools can be GPL-licensed **executables** invoked as test oracles; do not copy their implementation into BSD-licensed fsimg or the specification. All spec text, fixture generators, and format-specific logic should be independently authored or derived from openly documented format definitions.

Suggested test `Gemfile`:

```ruby
source "https://rubygems.org"

gem "minitest", "~> 5.0"
gem "rake", "~> 13.0"
gem "prop_check", "~> 1.0"
gem "bindata", "~> 3.0"

# Deliberately absent from the default bundle:
# gem "z3"  # optional, isolated, time-bounded later
```

Require Ruby 3.2+ to use `Data.define` in examples; use `Struct` instead if fsimg's test-tool support includes earlier Ruby releases. Ruby `pack`, `unpack`, and `byteslice` should often be preferred over a large BinData object for one or two fixed-offset fields.

### Cross-platform reality

The Ruby layer should run on **macOS, Linux, and FreeBSD**. The reference tools are optional adapters; the baseline deterministic tests should not require Linux. Linux CI can pin `dosfstools` and `mtools`. On macOS/FreeBSD, optionally support `newfs_msdos` and `fsck_msdosfs` with tool-specific capability flags. Do not assume that every platform's formatter can create every FAT geometry. If an oracle is unavailable, mark *that oracle* unavailable rather than silently skipping core tests.

## 2. Architecture: three distinct kinds of specification

```text
  FAT specification / interoperability notes / fsimg support policy
                           |
                     Ruby rule registry
                           |
         +-----------------+---------------------+
         |                 |                     |
   layout/encoding    semantic constraints   behavior policy
      (BinData)          (Ruby methods)      (accept/reject/safe)
         |                 |                     |
         +-----------------+---------------------+
                           |
                 Case/recipe generator
                  |                |
              valid cases     invalid mutations
                  |                |
                  +--------+-------+
                           |
                    image materializer
                           |
              +------------+-------------+
              |                          |
       fsimg C API/CLI               external oracles
              |                          |
              +------------+-------------+
                           |
              property checks + reporting
                           |
                  corpus / regression
```

Distinguish:

- **Physical layout:** bytes, offsets, endianness, bitfield packing, record length, and conditional structures.
- **Logical validity:** cross-field geometry; reserved regions; cluster chains, directories, filenames, file sizes, and allocation graphs.
- **Expected behavior:** what fsimg should do when a rule holds or fails, and which operation/stage must enforce it.

Binary layout code should generally **not reject invalid values by itself**. We need to be able to serialize those values to test the native parser. The logical rule predicates belong in a separate layer. Likewise, do not mix *format compliance* with *fsimg's explicitly supported subset*; record both separately.

### Behavior taxonomy

| Policy | Meaning | Example |
| --- | --- | --- |
| `must_accept` | Valid, supported image must parse/extract correctly | Standard FAT32 image with nonempty file tree |
| `must_reject` | Mandatory inconsistency must produce an error at an agreed stage | Zero bytes per sector, invalid region geometry |
| `safety_only` | Acceptance/rejection is ambiguous; must never crash/hang/write outside destination | Cross-linked cluster chains, arbitrary corruption |
| `equivalent_contents` | Different legal byte representations produce the same extracted tree | Relocated but equivalent valid cluster chain |
| `optional_feature` | Valid representation outside current support contract; never unsafe | Unimplemented legal extension |
| `diagnostic_only` | Report disagreement without failing conformance | Stale advisory FSInfo free-count value |

Always assert memory safety and bounded execution, even for `optional_feature` or `diagnostic_only`. A `must_reject` test should identify a **validation stage** (probe, open, list, extract, create), rather than assuming that recognizing a filesystem signature must fail as soon as any inaccessible file is corrupt.

## 3. Proposed source tree

```text
spec-test/
  Gemfile
  Gemfile.lock
  Rakefile
  README.md
  lib/fsimg_spec/
    rule.rb                # Named predicates, applicability and policies
    domain.rb              # Enum/range/bitmask with boundaries and witnesses
    case.rb                # Immutable test-case recipe and expectation
    mutations.rb           # Sector/field/graph mutation protocol
    image.rb               # Materialization, sparse files, temporary files
    filesystem_tree.rb     # Expected paths/content metadata/digests
    runner.rb              # fsimg adapter and bounded subprocess execution
    reports.rb             # Coverage, oracle results, failure records
    fixtures.rb            # Valid base image manifest and cache
    fat/
      bpb.rb               # BinData BPB, FAT32 extension, FSInfo
      geometry.rb          # Pure Ruby derived properties
      rules.rb             # FAT12/16/32 rule registry
      domains.rb           # Format-specific legal/illegal value domains
      constructors.rb      # Valid geometry/file-tree constructors
      patcher.rb           # BPB, FAT, directory and image-length edits
      allocation.rb        # Chain/cluster graph predicates
      generator.rb         # PropCheck recipes
      oracles.rb           # mkfs.fat, mtools, fsck.fat, etc.
  test/
    support/test_helper.rb
    spec/fat/
      test_layout.rb
      test_bpb_fields.rb
      test_geometry.rb
      test_fsinfo.rb
      test_fat_entries.rb
      test_allocation_graph.rb
      test_directories.rb
      test_create.rb
      test_extract.rb
      test_metamorphic.rb
      test_compatibility.rb
    regressions/
      test_saved_cases.rb
  fixtures/
    fat/
      bases/               # Small curated bases or reproducible recipes
      manifests/           # Stable JSON cases
      trees/               # Small expected file trees
  scripts/
    replay.rb              # Rebuild and rerun one case.json
    report.rb              # Print specification coverage
  tmp/                     # Untracked generated images and result artifacts
fuzz/
  ...                       # Existing native libFuzzer targets, unchanged
```

This may live under `tests/` in the existing repository rather than its own package. The modular split is conceptual; do not create empty modules simply to match the diagram. `runner.rb` must be the only layer with knowledge of actual fsimg CLI switches or C entry points.

## 4. Binary layouts with BinData

A short FAT32 BPB subset looks like:

```ruby
# lib/fsimg_spec/fat/bpb.rb
require "bindata"

module FsimgSpec
  module FAT
    class BootFields < BinData::Record
      endian :little

      string :jump_and_oem, length: 11   # offsets 0..10
      uint16 :bytes_per_sector           # 11
      uint8  :sectors_per_cluster         # 13
      uint16 :reserved_sectors           # 14
      uint8  :fat_count                  # 16
      uint16 :root_entries               # 17
      uint16 :total_sectors16            # 19
      uint8  :media                      # 21
      uint16 :fat_sectors16              # 22
      uint16 :sectors_per_track          # 24
      uint16 :head_count                 # 26
      uint32 :hidden_sectors             # 28
      uint32 :total_sectors32            # 32
      uint32 :fat_sectors32              # 36, FAT32 extension
      uint16 :extended_flags             # 40
      uint16 :filesystem_version         # 42
      uint32 :root_cluster               # 44
      uint16 :fsinfo_sector              # 48
      uint16 :backup_boot_sector         # 50
    end
  end
end
```

**Important:** `BootFields` models only the common BPB plus FAT32-specific fields through offset 51. Do **not** read its FAT32 extension into a FAT12/16 BPB and interpret those bytes as FAT32 fields. The production layout set should have a common BPB, mutually exclusive FAT12/16 vs FAT32 extensions, the remainder of the boot sector, signature bytes at offsets 510/511, FAT32 FSInfo and backup sectors, and 32-byte directory entries. Maintain offset assertions against the published format, independently of fsimg code.

For simple corruptions, patching bytes is clearer and faster than parsing/building the complete object:

```ruby
# Given a validated image pathname, set BPB_BytsPerSec at offset 11.
def write_u16le(path, offset, value)
  raise RangeError unless (0..0xffff).cover?(value)
  File.open(path, "r+b") do |f|
    f.seek(offset, IO::SEEK_SET)
    f.write([value].pack("v"))
  end
end

write_u16le("mutated.img", 11, 1_000)
```

For an in-memory test input, `bytes[11, 2] = [value].pack('v')` suffices. A malformed boot sector may be shorter than a complete BinData record; the harness must be able to create and pass that truncated input without BinData first throwing an exception.

## 5. Executable rule and domain model in Ruby

Keep the rule interface intentionally small. The following is a design sketch, not an additional DSL:

```ruby
module FsimgSpec
  Rule = Data.define(
    :id, :applies_to, :predicate, :on_failure, :source, :notes
  )

  class Domain
    attr_reader :legal, :invalid_examples

    def initialize(legal:, invalid_examples:)
      @legal = legal.freeze
      @invalid_examples = invalid_examples.freeze
    end

    def legal?(value) = @legal.include?(value)

    # The explicit lists supply determinism and boundary coverage;
    # PropCheck supplements them with random exploration.
  end
end

SECTOR_SIZE = FsimgSpec::Domain.new(
  legal: [512, 1_024, 2_048, 4_096],
  invalid_examples: [0, 1, 511, 513, 1_000, 4_095, 65_535]
)

SECTOR_SIZE_RULE = FsimgSpec::Rule.new(
  id: "fat.bpb.bytes_per_sector",
  applies_to: ->(_model) { true },
  predicate: ->(model) { SECTOR_SIZE.legal?(model.fetch(:bytes_per_sector)) },
  on_failure: :must_reject,
  source: "Microsoft FAT specification: BPB_BytsPerSec",
  notes: "Supported baseline sector sizes; separate unusual tool-specific extensions"
)
```

Use stable rule IDs, as they become regression identifiers. Each rule in the production registry needs:

- **Applicability:** FAT12/16/32, feature flags and preconditions; optional support profiles.
- **Predicate:** a pure Ruby function evaluated against a parsed/constructed *model*, not the fsimg implementation.
- **Domain or mutation factory:** how to generate valid witnesses and targeted invalid witnesses.
- **Expectation:** per operation, since probing, listing and extracting can differ.
- **Spec provenance:** document/link and relevant section; distinguish normative and observed tool behavior.
- **Dependency metadata (optional):** identify correlated constraints to help classify unavoidable multi-rule failures.

Avoid arbitrary DSL expression strings and Ruby `eval`. Procs/methods are sufficient and straightforward to debug. Only rule metadata and inputs—not executable predicates—are serialized to JSON.

### What to call valid versus supported

Maintain two answers for each test image:

1. **Spec conformance:** Is the representation valid under the documented format and relevant compatibility interpretation?
2. **fsimg support contract:** Is the format/variant/feature currently promised by fsimg?

For example, `mkfs.fat` itself can support unusual sector sizes that the initial fsimg profile may intentionally exclude. Do not silently redefine the normative specification to match an implementation policy. Record `unsupported_valid` separately from `invalid`. FAT12/16/32 classification is derived from the *number of data clusters*, not the `FAT32` text label.

## 6. Constructively generating valid FAT images

Most early tests should **not** solve arbitrary field values and hope a valid image emerges. Construct a valid geometry first, compute dependent fields, and render a consistent image.

For FAT12/16/32, the core arithmetic is:

```ruby
def ceil_div(a, b)
  raise ArgumentError, "nonpositive divisor" if b <= 0
  (a + b - 1) / b
end

def geometry(b)
  bps = b.fetch(:bytes_per_sector)
  spc = b.fetch(:sectors_per_cluster)
  return nil unless bps.positive? && spc.positive?

  root_dir_sectors = ceil_div(b.fetch(:root_entries) * 32, bps)
  total_sectors = b.fetch(:total_sectors16).nonzero? || b.fetch(:total_sectors32)
  fat_sectors = b.fetch(:fat_sectors16).nonzero? || b.fetch(:fat_sectors32)
  reserved = b.fetch(:reserved_sectors)
  fat_count = b.fetch(:fat_count)

  data_sectors = total_sectors -
    (reserved + fat_count * fat_sectors + root_dir_sectors)
  return nil unless data_sectors.positive?

  clusters = data_sectors / spc
  fat_type = if clusters < 4_085
    :fat12
  elsif clusters < 65_525
    :fat16
  else
    :fat32
  end

  entry_bits = { fat12: 12, fat16: 16, fat32: 32 }.fetch(fat_type)
  required_bytes = ceil_div((clusters + 2) * entry_bits, 8)

  {
    type: fat_type,
    clusters: clusters,
    data_sectors: data_sectors,
    fat_capacity_ok: fat_sectors * bps >= required_bytes
  }
end
```

This is an **illustrative derived-geometry check, not a complete validator**. Other checks must establish correct FAT12/16/32-specific field usage, actual image length and addressing, boot-sector placement, valid reserved areas and overlap rules, maximum representable FAT entry values, root directory layout, cluster-size limits, and feature flags. Evaluate over a validated model so a zero divisor or contradictory total-sector field does not produce an unhandled Ruby exception.

**Important exact FAT thresholds:** fewer than 4,085 data clusters is FAT12; 4,085 through 65,524 is FAT16; 65,525 or more is FAT32. These boundary configurations are especially useful as *spec-level parser tests*, but some real implementations recommend avoiding exact boundary sizes; interoperability expectations may need a separate test policy.

### Two categories of valid witness

**Formatter-supported cases:** Invoke `mkfs.fat` (or another platform formatter) with selected geometry, optionally populate a file tree with `mtools`, check the resulting BPB, and cache it. The formatter must not silently substitute another geometry or FAT variant; parse and verify the generated image before accepting it as a witness.

**Formatter-unusual cases:** Construct a smaller canonical fixture with a deliberately chosen on-disk variation that maintains all relevant predicates, or eventually implement a minimal *independent* FAT fixture builder for boundary configurations that formatters refuse. Mark these as *independent-construction* cases; validate against the rule model before using them against fsimg.

Do **not** take an existing 512-byte-sector image and rewrite the BPB to 4096 bytes per sector and then call it a *valid* 4096-byte-sector image. The geometry and all physical region offsets must be rebuilt or translated coherently.

## 7. Case recipes and mutation protocol

A case is a small, reproducible **recipe**. A sample JSON artifact:

```json
{
  "schema_version": 1,
  "id": "fat32.bpb.bytes_per_sector.invalid.1000",
  "format": "fat32",
  "profile": "baseline",
  "base_id": "fat32-64mib-512b-1spc-v1",
  "base_sha256": "computed-at-fixture-build-time",
  "operation": "probe",
  "mutations": [
    {"op": "set_u16le", "offset": 11, "value": 1000}
  ],
  "intended_violation": "fat.bpb.bytes_per_sector",
  "actual_violated_rules": ["fat.bpb.bytes_per_sector"],
  "expected": "must_reject",
  "provenance": "Microsoft FAT specification, BPB_BytsPerSec"
}
```

`base_sha256` above is a **manifest field placeholder**, to be populated with a real SHA-256 of a pinned fixture. Never persist a literal placeholder as a real test manifest. Additional optional fields: `tree_id`, `source_revision`, `runner_version`, `tool_versions`, `random_seed`, `expected_stage`, `resource_limits`, and `supports`.

Core mutation types (start with only the first two):

```text
set_u8(offset, value)
set_u16le(offset, value)
set_u32le(offset, value)
replace_bytes(offset, bytes)
truncate(new_length)
set_fat_entry(table, cluster, value)
set_directory_entry(directory_id, index, fields)
splice_chain(file_id, next_cluster)
relocate_chain(file_id, new_cluster_ids)
```

For each mutation, maintain bounds and size limits. Mutation code must be capable of writing deliberately invalid encodings without a semantic validator refusing to serialize them. General graph mutation need not be implemented before milestone B.

### Three generated-case classes

**A. Valid:** Generate a consistent geometry and logical file tree; materialize a valid image; check all applicable validity predicates; verify fsimg's acceptance and content correctness.

**B. Targeted invalid:** Start from a validated base, mutate one value/relationship, and reevaluate the registry. If the intended predicate fails and **all other applicable predicates** pass, label it `single_rule_invalid`. If multiple predicates necessarily fail, label it `multi_rule_invalid` with the actual set. Never claim a clean single-rule test when a mutation also invalidates dependent geometry.

**C. General malformed/fuzzed:** Combine mutations, use truncations and arbitrary byte edits. Only broad safety expectations may be defensible. Always record whether the generated input is rejected, accepted, or ignored; do not infer spec compliance from acceptance alone.

The targeted-invalid construction is the main differentiator from simple byte fuzzing. It makes failures explainable.

## 8. Using PropCheck: generation and shrinking

PropCheck generates and shrinks **input parameters and recipes**, not necessarily entire raw disk images. This makes shrinking meaningful: reducing a directory to one file and a cycle to two clusters is a better minimal counterexample than arbitrarily zeroing image bytes.

Minimal idiomatic example, combining deterministic and property-generated inputs:

```ruby
require "minitest/autorun"
require "prop_check"

class TestFatSectorSizeDomain < Minitest::Test
  G = PropCheck::Generators
  LEGAL = [512, 1_024, 2_048, 4_096].freeze
  BAD = [0, 1, 511, 513, 1_000, 4_095, 65_535].freeze

  def test_known_boundaries
    LEGAL.each { |n| assert_includes LEGAL, n }
    BAD.each   { |n| refute_includes LEGAL, n }
  end

  def test_generated_invalid_witnesses
    generator = G.one_of(*BAD.map { |n| G.constant(n) })
    PropCheck.forall(generator) do |n|
      refute_includes LEGAL, n
      # Full integration test: apply a BPB patch to a valid fixture,
      # run the fsimg adapter, and assert the rule-specific behavior.
    end
  end
end
```

The domain-only example validates the **test definitions**. It does **not** count as evidence of fsimg correctness. The actual behavioral test must invoke fsimg through the real adapter and compare its result against the rule. Separate these two coverage categories to prevent misleading reports.

Use `Generators.one_of`, `constant`, `choose(range)`, `tuple`, `array`, `map`, and `bind` for small structured models. Favor `map` from legal primitives and constructive recipes over `.where` filters that reject most generated samples. Ensure shrinkers preserve validity when the test requires valid data; supplement PropCheck's generic shrinker with recipe-specific reductions for chains, files, and directory trees.

Use explicit bounded example counts. A quick PR suite can run many cheap in-memory checks, but should never run an external 64 MiB `mkfs.fat` invocation for every random integer. Share immutable formatter fixtures and batch or sample end-to-end cases.

### Deterministic cases are mandatory

PropCheck is not a replacement for enumeration. Generate every small enumerated legal value, documented reserved sentinel and important adjacent boundary. The PR suite must test the exact FAT thresholds and FAT entry marker transitions regardless of which random values are drawn.

## 9. Initial FAT rules and coverage matrix

The initial rule registry should cover the following **representative** groups; decide precise normative/compatibility predicates individually and cite the source before assigning `must_reject`:

| Area | Rule examples | Positive / negative cases |
| --- | --- | --- |
| Common BPB | bytes per sector, sectors per cluster, reserved count | Enumerated supported sizes; zero/non-power-of-two/out-of-range; constraints on cluster size |
| FAT count | nonzero FAT count and compatibility profile | One/two FATs with consistent layout; impossible count/geometry |
| FAT variant | derived cluster count | 4,084 / 4,085 and 65,524 / 65,525 data clusters; beware real-world boundary behavior |
| Type-specific BPB | FAT32 zero root-entry count and FAT16-size field; FAT12/16 root placement | Valid variants and inconsistent extension fields |
| Size selection | `TotSec16` vs `TotSec32`; `FATSz16` vs `FATSz32` | Correct encoding and contradictory/zero totals |
| Geometry arithmetic | FAT region fit, required number of FAT entries, data region, image bounds | Tight-fit valid cases; undersized FAT; overflow; overlaps; truncated images |
| FAT entries | unused, allocated, reserved/bad, EOC and FAT32 high nibble | Masking behavior; valid chain ends; out-of-range pointers |
| FAT copies | active/mirrored FAT selection, consistency | Meaningful differences consistent with flags; corrupt unselected copy where permitted |
| FAT32 root | root cluster valid and allocated | Root at valid cluster; root outside data region; root referencing invalid chain |
| FSInfo | signatures, unknown/stale free-count, next-free hint | Hint semantics; don't reject solely because an advisory count is stale |
| Backup boot | optional/required placement as specified, primary vs backup disagreements | Correct backup; missing/corrupt backups with behavior policy |
| Directory | 32-byte entries, terminators, deleted entries, dot entries, 8.3 names | Legal directories; malformed attributes or references |
| Long file names | LFN ordering, sequence checksum, UTF-16 names | Valid multi-entry LFNs; wrong order/checksum; malformed Unicode |
| Allocation graph | fragmentation, EOC, loops, crosslinks, invalid cluster, file length/chain mismatch | Correct extraction or safety failure; no infinite loops |
| File contents | empty, small, exact-cluster, multi-cluster, sparse-in-logical-tree cases | Byte-for-byte tree equality and hashing |

**Suggested milestone A seed:** approximately 20–25 named BPB/geometry rules spanning FAT12/16/32, but only after writing down their source and behavior expectation. Prefer 12 correct and well-isolated rules to 30 assumptions treated as normative.

### High-value metamorphic properties

- Copy an allocated chain to different free clusters and update its references; extracted contents must remain identical.
- Reorder independent files/directory slots where the format permits it; logical tree remains the same.
- Change ignored high four bits of FAT32 allocation entries; interpretation of the low 28 bits must remain unchanged.
- Change advisory FAT32 FSInfo counts/hints without modifying the actual allocation table; extraction contents should not change.
- Permute physical file creation order while preserving the expected logical tree; the extracted tree is equivalent.
- Vary permitted volume labels/boot-code bytes without changing file contents; behavior must follow the format rules.

A metamorphic mutation must itself be checked for validity; otherwise a failing equivalence test could simply reflect a malformed transformation.

## 10. Independent functional oracles

Use multiple sources of truth **by test class**:

| Case | Primary assertion | Secondary evidence |
| --- | --- | --- |
| Externally generated valid image → fsimg reader | Extracted tree equals the generator's original tree (paths, type, sizes, SHA-256) | mtools also reads it; optional other reader |
| fsimg-created image → external reader | mtools extracts the original tree's contents | `fsck.fat -n` diagnostic; comparison with an independent reader |
| Valid noncanonical image | Normative rule + known model contents | Another implementation if it supports this case |
| Targeted invalid image | Predicate violation and agreed error policy | `fsck.fat -n` diagnostics only; not an absolute oracle |
| Compound malformed image | No sanitizer findings, hangs, unsafe writes or OOM | Other tools executed under limits, optionally |
| Metamorphic image pair | Same logical tree and observable property | Independent reader pair if helpful |

Never infer correctness from `fsimg create → fsimg extract` alone: both paths can share the same bug. Likewise, a filesystem checker can flag conservative incompatibilities or tolerate errors; its result must not override the published format rule.

### Canonical FAT fixture example

```sh
# Linux CI, with dosfstools and mtools installed.
# -C takes size in 1024-byte blocks; 65536 blocks = 64 MiB.
mkfs.fat -C -F 32 -s 1 base.img 65536
printf 'hello\n' > HELLO.TXT
mcopy -i base.img HELLO.TXT ::HELLO.TXT
mdir -i base.img ::
fsck.fat -n base.img
```

Production fixture code must inspect the resulting BPB, check that the expected type is *actually* FAT32, record formatter/tool versions and source-tree digest, and detect nondeterministic labels/timestamps/serial numbers where relevant. Once generated, cache the fixture by **content hash and formatter options**. Freeze or normalize incidental metadata for stable comparisons.

On macOS or FreeBSD, use an available formatter adapter or materialize committed recipe-based fixtures. External utilities should be independent of fsimg's codebase and run **without mounting**.

## 11. Runner, classification, isolation and safety

A `Runner` adapter should present a small interface, independent of the current fsimg CLI syntax:

```ruby
# Illustrative protocol. Bind to existing fsimg commands/API in runner.rb.
class Runner
  def probe(image:)             = raise NotImplementedError
  def list(image:)              = raise NotImplementedError
  def extract(image:, into:)    = raise NotImplementedError
  def create(tree:, image:, format:, options: {})
    raise NotImplementedError
  end
end
```

Each method returns a structured result, not just `true`/`false`:

```text
Result:
  status: accepted | rejected | unsupported | crashed | timed_out |
          resource_exceeded | sanitizer_failure | ambiguous
  stage: probe | open | list | extract | create
  exit_status: ...
  signal: ...
  duration_ms: ...
  stdout/stderr (bounded): ...
  extracted_tree_hash: ...
  output_file_count: ...
  resource_usage: ...
```

Do **not** assume fsimg exposes or needs a new CLI solely for testing. Prefer an existing C API exposed through a tiny native harness for high-volume in-process parser tests, and invoke the existing fsimg CLI for end-to-end tests. Ruby FFI is unnecessary unless it demonstrably speeds development; a subprocess is simpler and safer for corrupt images.

**Timeouts:** Run external tools with process-group termination, CPU/wall-clock limits, memory limits where supported, and bounded output capture. Do not rely on Ruby's `Timeout.timeout` alone to terminate a child process tree. Use isolated temp directories with path allowlisting and an extraction destination outside the fixture directory. Fail closed on path traversal, symlink escapes and out-of-root writes.

**Host safety:** Never mount arbitrarily mutated images through the host kernel; never run external checkers on random host block-device paths. Execute fuzz/reference processes under a low-privilege account or sandbox where feasible. An untrusted image may attack an oracle too.

**File sizes:** Use sparse images or shared read-only base images. For patching, prefer a per-test temporary copy for initial simplicity; optimize using reflinks/clonefile or a copy-on-write block overlay only after profiling. A 64 MiB image per cheap scalar property iteration would defeat the budget.

## 12. Spec coverage reporting

Generate JSON plus a readable report. Report **exercised behavior**, not merely the presence of Ruby predicates.

```json
{
  "rule_id": "fat.bpb.bytes_per_sector",
  "format": "fat32",
  "valid_witnesses_generated": 4,
  "invalid_witnesses_generated": 7,
  "boundaries_exercised": [0, 1, 511, 512, 513, 1024, 2048, 4096],
  "single_rule_isolation": "partial",
  "operations": ["probe", "extract"],
  "fsimg_results": {"passed": 10, "failed": 1},
  "oracle_results": {"mtools": "sampled", "fsck_fat": "diagnostic"},
  "last_regression": null
}
```

The numbers above are **illustrative**, not measured. Record actual attempted/passed/failed/skipped counts, and keep the following states distinct:

- Rule implemented but not exercised.
- Valid witness available / unavailable.
- Targeted invalid witness available / provably impossible / not yet generated.
- Behavior asserted against fsimg / only the independent Ruby predicate tested.
- Fixture accepted by formatter / unsupported by reference tool.
- Oracle agrees / disagrees / not applicable / unavailable.

A coverage report of 100% rule declarations is meaningless if only half the rules have reached fsimg or if all bad inputs violated many unrelated conditions.

## 13. Failure shrinking and reproducibility

Use PropCheck's shrinker first on **recipes**. Add very small domain-specific transformations in milestone B:

- Remove unrelated files/directories while retaining the failure.
- Decrease file size and number of clusters.
- Reduce a cycle to its shortest failing form.
- Reduce a cross-link to two files and one shared cluster.
- Remove unrelated edits until only those necessary remain.
- Move values toward an applicable numeric boundary without making the witness valid.

Every final failure should save:

```text
tmp/spec-results/failures/<case-id>/
  case.json           # stable versioned recipe and assertion
  fixture.json        # base image options/hash and tool versions
  fsimg.stdout        # bounded
  fsimg.stderr        # bounded
  oracles.json        # independent tool observations
  result.json         # observed status, timings, sanitizers
  image.img           # optional, only if size policy allows
  replay.sh           # exact, sanitized regeneration/replay command
```

A random seed is helpful but **not enough** to reproduce a failure across upgrades of PropCheck or external formatters. Save explicit field values/graph operations and the base fixture hash, then promote confirmed defects into deterministic regression manifests. Prefer recipes over committed multi-megabyte images.

## 14. Three execution loops, four CI profiles

### Inner loop: existing C/libFuzzer

Keep native targets and sanitizers. Seed with representative small valid and invalid metadata fixtures and minimized failures. Code coverage remains the fuzzer's optimization objective. Avoid feeding it hundreds of full 64–256 MiB image buffers with naive byte mutation.

### Middle loop: Ruby unit/property tests

Run thousands of cheap predicate/field/model tests in memory. Invoke the native reader/parser through an adapter for selected cases. End-to-end CLI tests are fewer and share fixtures. Minitest orchestrates suites; PropCheck generates and shrinks cases.

### Outer loop: independent compatibility

Sample whole-image cases and run mtools, formatters and checkers. Store cross-tool disagreements. These are slower, require pinned tools, and must not block inner-loop coverage.

| CI profile | Contents | Proposed bounded budget (tune by measurement) |
| --- | --- | --- |
| `test:spec:quick` | Deterministic field boundaries, 100–300 cheap PropCheck examples, 10–30 native image operations | Aim 2–3 minutes per PR |
| `test:spec:san` | Replay curated native regression seeds and run bounded fuzzing | About 30–120 seconds per target in a separate job |
| `test:spec:nightly` | Thousands of Ruby cases, 50–150 sampled external-oracle operations, longer fuzz campaigns | Aim 10–30 minutes |
| `test:spec:deep` | Large volumes, unusual layouts, broad mutation matrices, extended differential checks | Explicit opt-in; no PR requirement |

These are *target budgets*, not claims of measured throughput. Enforce ceilings for image capacity, number of files, max path depth, memory, output, and per-case runtime. The minimal legal FAT32 image has 65,525+ data clusters, so sparse, tens-of-MiB logical images remain relevant even if most bytes are never written.

### Command-line use (without a new test runner)

```sh
bundle install
bundle exec rake test:spec:quick
bundle exec rake test:spec:nightly
bundle exec ruby -Ilib:test test/spec/fat/test_geometry.rb
ruby scripts/replay.rb fixtures/fat/manifests/example.json
ruby scripts/report.rb tmp/spec-results/spec-coverage.json
```

Define these Rake tasks in the project; they are **proposed entrypoints**, not commands that already exist. Existing fsimg executable/library paths should be set through `FSIMG_BIN` or equivalent configuration. Generate a machine-readable report and upload it as CI artifact. Run C sanitizer jobs separately so their crashes cannot corrupt the Ruby coordinator.

## 15. Native libFuzzer integration and structured recipes

**Milestone A:** No changes to libFuzzer beyond importing representative generated regression seeds.

**Milestone C (only if coverage analysis warrants it):** Fuzz a compact instruction stream that refers to an immutable base image, rather than mutating megabytes of disk bytes:

```text
recipe := base_id, operation_id, bounded_patch_count, patches...
patch  := set_bpb(field_id, u32)
       | set_fat_entry(cluster_id, u32)
       | change_file_size(directory_entry_id, u32)
       | truncate(logical_image_size)
```

The C harness applies bounded patches through a **copy-on-write sector overlay** and uses the existing fsimg reader/block-I/O interface. Ruby generates initial recipe corpora and decodes minimized failures for reporting. The native harness does not call Ruby inside each libFuzzer iteration; that would slow feedback and complicate sanitization.

Assess the improvement in native code coverage and crash discovery *per CPU minute* against ordinary byte mutation before committing to this extra complexity. Do not introduce new image/virtual-disk abstractions in fsimg solely for speculative fuzzing.

## 16. When Z3 would actually help (optional)

Z3 is not the test engine. **Its role is synthesizing small sets of numeric field values** under cross-field constraints.

Potentially useful queries:

1. **Valid numeric witness:** Find `total_sectors`, `fat_sectors`, `reserved_sectors`, and `sectors_per_cluster` producing exactly 65,525 clusters with enough FAT capacity.
2. **Single-rule invalid witness:** Satisfy all chosen predicates except `fat_capacity_ok`, proving that a failure is attributable to that one rule (if satisfiable).
3. **Boundary reachability:** Determine whether a requested combination is even possible under profile limits, rather than wasting random test generation cycles.
4. **Contradictory rules:** Flag a set of spec predicates that cannot be satisfied together.
5. **Rare interactions:** Generate a viable geometry with mutually constraining fields that standard formatters cannot express directly.

Conceptually:

```text
valid:        C1 AND C2 AND ... AND Cn
break rule k: C1 AND ... AND NOT Ck AND ... AND Cn
```

If an isolated failure is `unsat`, do **not** force one; it may be mathematically impossible to violate only that predicate. Record an unsatisfiable target or generate a *declared* multi-rule corruption. Distinguish solver `unknown`/timeout from `unsat`.

**Solver policy if implemented later:**

- Keep the solver behind an optional `require "z3"` and use only a dozen or so bounded scalar integer/bit-vector variables; don't encode all FAT entries, directory trees, or file contents.
- Solve in a **separate subprocess**. Ruby Z3 bindings wrap native code; a subprocess also bounds native memory issues.
- Per-query wall-clock timeout of roughly **100–200 ms**, aggregate PR solver budget of **~2 seconds**, and no retry loops on timeout. Tune if justified by measured wins.
- Cache concrete SAT witnesses in versioned JSON so normal CI can replay without Z3 installed.
- Recheck all returned values using the independent Ruby rule registry; a mis-modeled SMT formula must not become the oracle.
- On timeout/unknown, fall back to constructive Ruby generation or curated fixtures and report the coverage gap.

**Decision gate:** Only integrate Z3 after documenting at least **three** recurring, important constraint families that are hard to generate constructively and demonstrating a measurable coverage improvement. For FAT12/16/32, expect **not to need it**.

## 17. Milestones and acceptance criteria

### Milestone A — Minimal useful FAT spec harness

Deliver:

1. Gemfile/Rake/Minitest/PropCheck/BinData setup and native fsimg adapter.
2. Common FAT BPB layouts and correct FAT12/16 vs FAT32 extension selection.
3. `Rule`, `Domain`, `Case`, field-patch mutation and JSON reporting abstractions.
4. About 15–25 documented BPB/geometry rules (include FAT cluster count thresholds and FAT capacity).
5. A few cached, independently formatted valid FAT12/16/32 fixtures; one representative populated FAT32 tree.
6. Exhaustive tests of small legal domains and explicitly listed invalid boundaries.
7. Selected end-to-end probe/extract tests; read-only `fsck.fat` for valid output; versioned failures.
8. CI quick profile, with meaningful spec-coverage JSON.

**Exit tests:**

- Changing a rule's allowed-value set changes generated cases without editing per-case test files.
- At least one targeted invalid input fails its selected predicate; the report names any collateral violations.
- fsimg behavior (not only Ruby predicate evaluation) is asserted for representative valid and invalid cases.
- The same saved recipe replays exactly against a fixed base image and reports identical relevant bytes.
- No Z3 or custom native fuzz target is required.

### Milestone B — Filesystem semantics and writer/reader oracles

Deliver:

- Small original-tree model with deterministic content hashes.
- Valid files and nested directories populated via independent tools.
- Semantic FAT-chain operations: allocate, fragment, split, loop, cross-link, out-of-range, truncate.
- Directory-entry, LFN and FSInfo mutations, with normative/diagnostic expectation policies.
- `fsimg create → independent extract` tests; `external create → fsimg extract` tests.
- First semantic shrinkers and metamorphic checks.

**Exit tests:** Detect and reproduce an injected cluster-chain cycle, a crosslink and an undersized FAT without sanitizer findings, infinite loops or false positive 'valid' classifications. Demonstrate correct extraction of at least one fragmented file and LFN example.

### Milestone C — Optimize fuzzing from coverage evidence

Deliver:

- Generation of small, meaningful native libFuzzer seeds from saved recipes.
- Crash and sanitizer regression promotion, deduplication by normalized failure.
- Profiling of Ruby invocation cost, image materialization and native coverage.
- Optional C patch-recipe target + sparse/COW block overlay **only if justified**.

**Exit tests:** Demonstrable coverage gain or new regression discovery under equivalent CPU budgets compared with existing byte-level fuzzing. Avoid shipping a second native harness with no measurable value.

### Milestone D — Another filesystem as validation of generality

Port **exFAT or QNX6FS** by implementing format-specific layouts, predicates, recipe constructors and tool adapters while reusing the rule/case/runner/report infrastructure. Extract additional abstractions only after that port exposes real duplication. A future HFS+ or APFS target may need more elaborate graph models and generation; do not front-load them into the FAT framework.

**Exit tests:** Adding the second filesystem does not require changing the core test runner's rule semantics or report schema, except through a documented version bump.

### Concrete first pull request

If implementing one PR before any broader refactoring, keep it to:

```text
Gemfile / Rakefile
lib/fsimg_spec/{rule,domain,case,runner,reports}.rb
lib/fsimg_spec/fat/{bpb,geometry,rules,patcher}.rb
test/spec/fat/{test_bpb_fields,test_geometry}.rb
fixtures/fat/manifests/
```

Use a verified 64 MiB 512-byte-sector FAT32 fixture plus small FAT12 and FAT16 reference fixtures. Cover `bytes_per_sector`, `sectors_per_cluster`, reserved count, FAT count, root entry fields, total/FAT sector fields, cluster-count class, FAT capacity and root cluster bounds. No directories/graph synthesis, solver, DSL, native custom mutator, or virtual disk abstraction in this PR.

## 18. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Ruby spec and fsimg share the same incorrect interpretation | Independently author spec rules; cite published docs; cross-check with external writer/reader and real fixtures |
| Validator rejects legitimate noncanonical images | Distinguish normative rules, supported features and advisory metadata; use `safety_only`/`diagnostic_only` when appropriate |
| Single-field edit unexpectedly invalidates many fields | Re-evaluate all applicable predicates and report true violation set |
| Time spent generating huge images | Cache formatted bases, separate cheap predicates from image operations, bound CI and use sparse images |
| PropCheck shrinker changes a valid witness into an invalid one | Construct validity-preserving recipes; verify predicates after every shrink and before native execution |
| Random tests miss normative boundaries | Enumerate small domains and exact threshold neighbors deterministically |
| Reference tools disagree | Capture versions and results; specification is authoritative where clear; triage divergences |
| Native runner hangs or corrupts filesystem | Low-privilege subprocesses, process-group termination, wall/memory/output limits, temporary directory isolation |
| Reproducibility breaks across updates | Pin dependencies, save concrete recipes and base image hashes, promote failures to regressions |
| Generic framework balloons in size | Build only abstractions exercised by FAT; verify reuse on the second filesystem before generalizing |
| Licensing contamination of BSD-oriented project | Keep independent rule implementations and only invoke GPL tools externally; record sources, do not copy proprietary or incompatible code |

## 19. Definition of done and proposed success metrics

At completion of milestone A, aim to produce:

- A runnable Ruby quick suite with reliable, documented setup on at least Linux CI and the developer platform.
- A machine-readable specification coverage matrix with **actual test outcomes**, not vanity percentages.
- Deterministic valid and invalid boundary cases for every included FAT BPB rule.
- Independent validation of representative fsimg extraction and creation output where existing creation operations allow it.
- At least one minimized, reproducible negative image recipe.
- A zero-solver dependency test harness that remains comprehensible to the filesystem implementer.

At milestone B, quantify which failures were found by: deterministic rule boundaries, random PropCheck examples, semantic mutations, compatibility comparisons, and native libFuzzer. Keep coverage and defect classification separate; a test suite that says why a failure occurred is often more valuable than simply running more random iterations.

**Recommended starting action:** Implement milestone A as a stand-alone Ruby test tree in fsimg. Validate the rule/case/report interfaces against 2–3 real FAT32 bugs or intentionally injected faults before expanding into semantic graphs. Treat Z3 as a later, opt-in test-case synthesis backend—not part of the baseline architecture.

## 20. References

### Primary format reference

- Microsoft FAT specification (hosted PDF): https://www.scs.stanford.edu/~zyedidia/docs/_other/fat.pdf
- Follow the relevant normative sections for BPB, FAT type determination, FAT entries, FAT32 FSInfo, and directory entries. Distinguish compatibility observations from explicit spec requirements.

### Ruby libraries

- PropCheck: https://github.com/Qqwy/ruby-prop_check
- PropCheck generator API: https://www.rubydoc.info/github/Qqwy/ruby-prop_check/PropCheck/Generators
- PropCheck gem: https://rubygems.org/gems/prop_check
- BinData: https://github.com/dmendel/bindata
- BinData gem: https://rubygems.org/gems/bindata
- Minitest: https://github.com/minitest/minitest
- Ruby Z3 bindings (optional): https://taw.github.io/z3/
- Ruby Z3 gem: https://rubygems.org/gems/z3

### Native fuzzing and independent FAT tools

- LLVM libFuzzer: https://llvm.org/docs/LibFuzzer.html
- Structure-aware fuzzing: https://github.com/google/fuzzing/blob/master/docs/structure-aware-fuzzing.md
- dosfstools (`mkfs.fat`, `fsck.fat`): https://github.com/dosfstools/dosfstools
- GNU mtools: https://www.gnu.org/software/mtools/manual/mtools.html

### Prior related approaches (inspiration, not dependencies)

- EverParse: https://project-everest.github.io/everparse/
- FormatFuzzer: https://github.com/uds-se/FormatFuzzer
- Kaitai Struct: https://kaitai.io/
- Schemathesis: https://schemathesis.readthedocs.io/
- Janus (filesystem fuzzing): https://github.com/sslab-gatech/Janus
- Hydra (filesystem fuzzing): https://github.com/sslab-gatech/hydra

---

**Final decision:** Ruby (`Minitest + PropCheck + BinData`) for the test/spec layer; formatters and mtools/fsck as independent oracles; existing C/libFuzzer for coverage-guided fuzzing; optional Z3 *only* for measured difficult numeric constraints. Prefer small, explicit, reproducible recipes over monolithic image generation or solver-heavy infrastructure.
