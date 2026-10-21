# fsimg working agreement

fsimg is a BSD-2-Clause filesystem image tool and embeddable C11 library. The
command is `fsimg`; the library artifact is `libfsimg`. Read the concise
[guiding principles](specs/principles.md) and
[architecture and contracts](specs/architecture.md) before changing architecture
or interfaces. [specs/README.md](specs/README.md) indexes the authoritative
specification; [the implementation plan](docs/implementation-plan.md) orders
the work. Implementation follows the [style guide](specs/style.md).

## Collaboration

* Ask, don't assume. If something is unclear, ask before writing a single line.
  Never make silent assumptions about intent, architecture, or requirements.
* Flag uncertainty explicitly. If you are not confident about an approach or
  technical detail, say so before proceeding. Confidence without certainty
  causes more damage than admitting a gap.
* I'm always open to ideas on better ways to do things. Please don't hesitate
  to suggest a better way, or one that has long lasting impact over a tactical
  change.
* Security is paramount and on equal footing with correctness.
* **If you discover a bug, discrepancy, or something unexpected, fix it**
  whether or not it relates to the task at hand. Fix it if it is
  straightforward or required to make progress; otherwise document it in
  `TODO.md`.
* Read this file again after each context compaction.

## Licensing and dependencies

* New project-owned code uses the BSD-2-Clause license. Check provenance and
  license terms per file before importing code; preserve required copyright,
  attribution, and license notices. A reference implementation or test oracle
  is not automatically suitable for inclusion in the project.
* Keep the core library and default CLI free of third-party runtime
  dependencies beyond libc, host OS APIs and the system zlib. zlib is the one
  permitted library, required and never vendored: compressed DMG and gzip
  are basic layer features, its license is permissive, it is the system
  library on macOS and FreeBSD and standard on Linux, and inflate parses
  untrusted images, so the host's security fixes must apply. Optional
  integrations, such as libarchive import/export, belong outside the core
  and must not become requirements for ordinary image creation or
  extraction.
* Normal raw-image use requires no root privileges, mounts, kernel filesystem
  support, or external filesystem utilities. The CLI can isolate work in a
  restricted process running its own executable.

Development tools are a separate question. If a build or development tool is
missing, install it via homebrew or ask me to install it. Do not work around a
missing tool.

## Design

* Code should be simple and clean, never over-complicate things.
* **Judge complexity by the model a maintainer must understand.** Compare the
  concepts, entities, independently varying states, ownership rules and
  coordination obligations a change introduces with those it removes across
  the whole system. Lines of code are a useful proxy for review effort, not
  an acceptance gate. A larger module is a win when it replaces scattered
  mechanisms with one clear owner and fewer rules for its callers. Moving
  the same obligations behind a new interface does not remove them.
* **Human readability is a requirement.** Follow the
  [readability standard](specs/style.md#readability-standard): another
  developer must understand purpose, data flow, invariants and failure paths
  without reconstructing the implementation or reading the conversation.
* If a design starts getting too complicated or convoluted, or accumulates a
  lot of edge cases, stop and re-evaluate the big picture. A pile of special
  cases is usually a symptom that the architecture is wrong: step back and ask
  whether changing the architecture would make the edge cases disappear rather
  than handling them one by one. A simpler design that eliminates a class of
  problems beats a clever one that patches each instance.
* **Open-ended enumeration is the smell; "by construction" is the fix.** The
  warning is a correctness argument that depends on remembering every caller,
  execution path or exception that must participate in a protocol. Each new
  caller can invalidate that argument. An explicit enum or exhaustive switch
  over a closed set of states can instead make a design easier to understand;
  spelling out necessary cases is not itself a defect. When review keeps
  finding missing participants or exceptions, stop adding entries. Ask what makes
  the enumeration necessary at all, and change that instead, so the property
  holds *by construction* rather than by exclusion: make the illegal state
  unrepresentable, make the invariant a derived fact nothing can destroy, make
  the one funnel the only way to reach the operation. Two questions get you
  there quickly -- "what is this list protecting?" and "what would make the
  list empty?" Prefer the answer that deletes mechanism over the one that adds
  it; a fix that is wider but shallower (many mechanical call-site edits, each
  failing loudly if wrong) beats one that is narrow but deep (a small clever
  interlock whose failures are schedule-dependent).

## Filesystem image architecture

* Use **writer** for image creation and BSD-style `_t` typedefs for all
  project-defined C types. Keep readers and writers independent; neither is a
  mutable filesystem or an in-place editor.
* Follow the [shared contracts](specs/architecture.md#contracts). The resource
  context owns budgets, bounded I/O owns volume ranges, the extractor owns host
  paths, and the output coordinator owns publication. Drivers cannot bypass them.
* Bound metadata as well as payload memory. Use the specified metadata scratch
  storage and external algorithms; do not retain a complete FAT, wide directory,
  source catalog, or fragment index in RAM merely because payload copying streams.
* Keep source consistency, metadata fidelity, reproducibility, ownership, and
  failure semantics explicit. A partial output is not success. The library must
  not exit, print CLI diagnostics, or silently change its host's sandbox.

## Image safety and extraction

* Follow the [security specification](specs/design-security.md). Treat images
  and source trees as untrusted; validate arithmetic, encoding, geometry, and
  termination before using them. Initialize all emitted bytes and zero ranges.
* Extract beneath a private root with descriptor-relative operations and
  no-follow protections: a directory fsimg creates exclusively, or an existing
  empty directory the user owns, others cannot write, and no non-trivial ACL
  applies to, verified on its opened descriptor. Never merge into a populated
  tree, overwrite, or restore privileged metadata under the safe profile.
* The CLI uses an OS sandbox when available and warns otherwise. Required mode
  refuses unconfined operation. An unexpected setup failure is never a reason
  to silently retry without confinement. Sandboxing does not replace validation.

## Code

* Use portable C11, targeting FreeBSD, Linux, and macOS. Keep platform-specific
  behavior in a small compatibility layer rather than spreading it through
  filesystem drivers.
* Follow the [style guide](specs/style.md). It covers layout, naming, `_t`
  typedefs, comments, the BSD memory primitives, and `getopt(3)` flags.
* Watch the argument order of `bcopy(src, dst, n)`, the opposite of `memcpy`.
  Flag it in code review.
* Do not leave temporal or campaign-specific marks in code, comments, doc
  strings, specs or user-facing docs: no "now fixed", "previously broken",
  "as of <date>", "Wave 3", "Addressing BUG-12", "(fixed in commit abc123)".
  Code and docs describe the current state of the system; the why-and-when
  belongs in commit messages, which are read in temporal context.
* Source and build files, including C headers and shell scripts, must be
  ASCII-only. Markdown prose is exempt. Enforce this in build checks and CI.

## Ruby

Ruby is a development tool, confined to the specification-driven test harness
under `test/spec/` ([design](specs/design-spec-testing.md)). Write Ruby that
reads like the problem: functional, idiomatic and small. The syntax floor is
Ruby 3.2, so use `_1` rather than `it`; CI runs the floor version so the
floor is enforced, not assumed. Every file starts with the SPDX
BSD-2-Clause header and `# frozen_string_literal: true`, uses two-space
indentation, and is ASCII-only like other source files.

Prefer:

* Immutable values: `Data.define`, frozen constants and configuration.
* Pure functions and pipelines: `then`, `Enumerable` and `Enumerator::Lazy`,
  `filter_map`, `each_with_object`, `sum`, `tally`, `group_by`, `zip`. No index
  loops.
* Small objects with one job, and `call`-style callables for steps so they
  compose.
* Keyword arguments for anything with more than two parameters, including
  case construction. No positional boolean flags.
* Pattern matching (`case ... in`) for shape-dependent logic, such as decoded
  records and NDJSON payloads.
* Explicit errors under an `FsimgSpec::Error` hierarchy. Raise; do not return
  `nil` to mean failure. Return `nil` only for "absent", and say so in the
  name (`find_` versus `fetch_`). A total decoder is the exception: it marks
  absent or undefined fields explicitly instead of raising.
* Endless methods for one-liners.
* `private` by default; expose the smallest surface.
* Binary strings via `String#b`, and explicit-endian `pack`/`unpack`
  directives (`v`, `V`, `S<`, `L<`).

Avoid:

* Mutating arguments or shared state, and `!` methods on anything you did not
  just create.
* `unless ... else`, nested ternaries and `and`/`or` as control flow.
* Broad `rescue`. Rescue the specific exceptions an operation expects, such
  as an `Errno` value mapped to an answer. A catch-all rescue appears only at
  a declared boundary that records the error as a result, such as the
  harness's per-case boundary.
* Monkey patching core classes or gems; extend through wrapper objects.
* Classes that hold no state and only carry class methods. Write stateless
  operations as module functions.
* `eval`, `instance_eval` DSLs and `method_missing` on public interfaces. The
  harness has no rule DSL: rules are plain values and procs. BinData record
  declarations are the one sanctioned declarative DSL, confined to layout
  files.
* `Kernel#open`, `IO.read`, `IO.write`, `IO.readlines` and `IO.foreach` with
  a path built from data: a leading `|` runs a command. Use `File.open`,
  `File.read` and `File.binread`.

```ruby
# Bad
def legal_sizes(values)
  out = []
  for v in values
    out << v if v >= 512 && v <= 4096 && (v & (v - 1)).zero?
  end
  out
end

# Good
def legal_sizes(values) =
  values.select { power_of_two?(_1) && (512..4096).cover?(_1) }

private def power_of_two?(n) = n.positive? && (n & (n - 1)).zero?
```

Metaprogramming is allowed only to remove repetition that mirrors data, such
as accessors generated from a layout's field table, never to be clever.
Generated methods satisfy `respond_to?` and appear in `methods`; each
generated surface has a comment saying what it generates and a test that
calls a generated member. If plain code is within about 20% of the length,
write plain code.

Tests use Minitest and follow [readable tests](specs/style.md#readable-tests).
Test names describe behaviour, such as
`test_cluster_count_4085_selects_fat16`. Tests never reach the network.

## Tests and sanitizers

* Write a detailed test suite as you add features. It must be re-executed at
  every major change. Document the actual build, test, and sanitizer commands
  alongside the build system; report unavailable or skipped checks explicitly.
* When fixing a non-trivial bug, write a regression test FIRST that exercises
  the broken path and confirm it actually fails against the unfixed tree. Only
  then apply the fix and re-run the test to confirm it now passes. A fix
  without a failing-then-passing test is not a fix - it is a guess that happens
  to compile. Trivial typo and one-line obvious-correct fixes are exempt;
  anything involving image parsing, layout planning, extraction, memory
  ownership, or a multi-step code path is not trivial.
* Cover checked arithmetic, endian codecs, names and Unicode, geometry,
  allocation chains, directory sizing, checksums, and bounded I/O. Include
  malformed and truncated images, cycles, fragmentation, empty files and
  volumes, format limits, and names that collide on the target filesystem.
* Round-trip tests must create, read, enumerate, and extract images, comparing
  bytes and supported metadata. Verify reproducible output byte for byte and
  check that padding and unused regions contain no stale data.
* Validate generated images with independent implementations, and read images
  produced by other tools. For FAT and exFAT, use appropriate tools such as
  mtools, dosfstools, and exfatprogs as development/test tools, not runtime
  dependencies. Self-round-trips alone do not establish format correctness.
* Exercise extraction breakout attempts, symlink pivots, destination/source
  aliasing, changed sources, resource limits, and cancellation. Inject
  allocation failures, short I/O, premature EOF, and write/flush failures;
  verify cleanup and error reporting. Keep ordinary tests unprivileged and
  confine all writes to disposable fixtures.
* Fuzz format probes, on-disk parsers, allocation-chain traversal, directory
  enumeration, and filename conversion under sanitizers and finite limits.
* Run the full suite under AddressSanitizer before committing changes touching
  allocation, image parsing, I/O buffers, extraction, or library object
  lifetimes. Enable supported leak, use-after-return, and initialization-order
  checks; exercise allocation failure handling. Treat any ASan report as a
  HIGH finding and fix the root cause.
* Run UndefinedBehaviorSanitizer alongside AddressSanitizer when touching
  arithmetic, offsets, sizes, or bit-level code. Run ThreadSanitizer when
  introducing or changing concurrency or state shared between workers.
* Run standalone LeakSanitizer and MemorySanitizer on supported platforms.
  A skipped or unsupported sanitizer is not a passing check.
* Implementation subagents do not run sanitizer or fuzz suites. The
  coordinating session runs them once, on the integrated tree, before the
  work lands; subagents run the plain suite with `make -j32 check`.

### Linux and FreeBSD

Development happens on macOS. Check Linux and FreeBSD before work that touches
platform behaviour lands, such as the sandbox, host I/O, `compat/` or the
worker. Every report names the platforms that ran and those that did not.

* **Linux** runs in a container: `build-aux/linux-check.sh [-r REV]
  [COMMAND]` checks a committed revision in the environment of the CI Linux
  job. [docs/build.md](docs/build.md) gives its contract and the kernel its
  Docker VM needs for the sandbox profiles to run. LeakSanitizer and
  MemorySanitizer run there, not on macOS.
* **FreeBSD** runs on a FreeBSD host. The procedure is the commands inside
  the `su -c` string of the FreeBSD job in `.github/workflows/check.yml`, run
  as yourself with `gmake` for `make`: they install the spec gems and build
  the fatcat oracle first, and a run counts only when `test/spec.sh` passed
  rather than skipped.

## Committing

* Each solid progress should be committed in the git repository.
* Before committing code, test that what you produced is high quality and that
  it works:
  - Launch a code-review subagent **FIRST**, before running tests. Ask it to
    enumerate bugs, design issues, AND security / information-leak concerns.
    Frame the security pass explicitly in the prompt - don't assume the agent
    will check on its own.
  - Ask the reviewer to apply the style guide's
    [readability review](specs/style.md#readability-review). Require specific
    functions or passages as evidence; formatting, comment count, and passing
    tests are insufficient. Address readability findings before committing.
    For an architectural change, require a before/after comparison of the
    concepts and maintenance obligations added and removed across the system;
    a line-count delta alone does not establish simplification.
  - Address every CRITICAL and HIGH finding before committing. Note MEDIUM
    findings in `TODO.md` or the commit message if not fixed.
* The QEMU/QNX evidence harness and research scripts under `tools/qnx/` are
  exempt from the style guide and from the code-review and OpenCode review
  gates.
* Each implementation stage also receives an external, read-only review through
  OpenCode using the Z.ai coding-plan subscription, model
  `zai-coding-plan/glm-5.3`, and `--variant max`. This supplements the required
  code-review subagent and covers the same correctness, design,
  security/information-leak, and readability criteria. Re-review substantive
  fixes before committing the stage. Report an unavailable or failed external
  review explicitly; do not silently substitute another provider, model, or
  reasoning level.

## Documents

[specs/README.md](specs/README.md) indexes the authoritative specifications.
[specs/principles.md](specs/principles.md) states the guiding principles,
[specs/architecture.md](specs/architecture.md) owns cross-subsystem contracts,
and [specs/style.md](specs/style.md) holds coding conventions. Keep implementation
sequencing in [docs/implementation-plan.md](docs/implementation-plan.md), open
work in `TODO.md`, and release changes in `NEWS` when there is content to record.

* For every major feature, write `specs/design-<feature>.md` first and
  reference it from `specs/architecture.md` and `specs/README.md` before
  implementation.
* **Specs describe the target design, in the present tense.** Edit them when
  the design changes, not to record what shipped, what was deferred, or which
  parts are not built yet - that belongs in `TODO.md`. A design doc is never
  trimmed to match a partial implementation.
* A decision that constrains more than one subsystem is recorded, with its
  rationale, in the relevant architecture or contract section of
  `specs/architecture.md`; any other decision lives in the design doc it shapes.
  When a decision changes, edit it in place; git holds the history.
* `TODO.md` is split into categories (major initiatives, bugs by subsystem,
  security residuals, planned features). Each item has a `[Severity]` tag
  (Critical / High / Medium / Low) and a component pointer. Small items live
  inline as one- to few-line bullets. Larger initiatives fan out to category
  docs in `docs/todo-*.md` and are referenced from `TODO.md` by a
  one-paragraph summary. Each category doc follows the same rule recursively:
  small items inline, anything beyond about a page splits into its own file
  linked from the category doc. When an item is closed, delete it - git
  history is the record of completed work.
* `TODO.md` holds only work worth doing. Do not add an item for something
  not worth fixing, or for behaviour a man page or design doc already states
  as a contract or limitation. A limitation users should know about belongs
  in the user-facing docs (README, man pages), not in `TODO.md`.
