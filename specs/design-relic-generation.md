# Relic generation of tables and local predicates

## Purpose and ownership

Relic rules derive immutable compatibility tables and bounded predicates over
one decoded record or one supplied option record. Generation removes repeated
implications between independently maintained facts; it does not replace a
small table that already states its answer directly. A generated predicate has
the same private C interface and owner as the handwritten operation it replaces.
The public library API, worker protocol and driver registry acquire no rule
objects, runtime plugins or generator-specific versions.

This document owns the common rule-source, binding, diagnostic and generated
artifact contracts. [Generated verification](design-generated-verification.md)
owns lowering large image relations to bounded external operations. The
[architecture contracts](architecture.md#contracts) retain authority over
resources, byte boundaries, metadata policy and publication.
[Settings](design-settings.md), [metadata loss](design-metadata-loss.md), and
[driver contract tests](design-driver-contract-tests.md) retain their own
semantics; generation cannot redefine them by deriving a convenient table.

Relic owns language parsing, safety analysis, stratification, normalized rules,
and the restricted code-generation backend. fsimg owns its rule programs,
application bindings, a development-only generator executable, generated
artifacts, and the adapters connecting them to existing operations. The backend
is a Relic development interface, not an fsimg public plugin ABI. fsimg does not
include Relic's private AST or evaluator headers or copy its parser. Work on
that backend belongs in the Relic repository and receives its own review.

The target backend is distinct from the evaluator described by Relic's
`README.md`, `specs/design-api.md`, and `specs/design-planning.md`. An evaluator
plan is not a portable generation interface: `lib/plan.h` carries resident
relation views, interned value identities and indexes. Neither serialization
of those pointers nor parsing human-readable `-e` output defines the boundary.
Implementation availability and ordering belong in the
[implementation plan](../docs/implementation-plan.md), not in rule semantics.

## Artifacts and authoritative inputs

Each adopted family has these artifacts, with one owner for each fact:

| Artifact | Content and authority |
| --- | --- |
| `specs/rules/<family>.dl` | Ordinary Relic facts and rules; format constraints cite their authoritative format specification in comments |
| `tools/relic/<family>-bindings.c` | Constant C descriptors binding predicate columns, exported answers and diagnostic identities to fsimg types and owners |
| `tools/relic/generate.c` | Development-only driver of the Relic API and deterministic artifact production |
| `lib/<owner>/generated/<family>.h` and `.c` | Checked-in private C11 declarations, tables and functions compiled by ordinary builds |
| `lib/<owner>/generated/<family>.manifest` | Canonical generation inputs, generator identity, backend contract, output digests and operation/storage summary |
| Independent fixtures and tests | Hand-authored expectations and external evidence; never an output of the production rule program |

Paths are repository relative. Generated headers are not installed. A generator
unit explicitly lists all its source files; there are no directory globs whose
contents silently enlarge a rule program. Relic text has no fsimg directives,
embedded C expressions, schema annotations or file-opening instructions.
Bindings are ordinary C constant initializers under the fsimg style guide, not
a second parsed language. Ruby remains confined to `test/spec/`.
Rule files carry the project copyright and SPDX notice in Relic `%` comments.

A family exports named predicates and tables. Private intermediate predicates
are free to change. Exports use stable semantic names, including separate
violation predicates where diagnostic distinctions matter; a rule's line number
is provenance, not a diagnostic identity. Renaming an export changes the binding
and its tests in the same change. Rule source comments identify the property,
units and domain; generated code preserves that information near the operation.

A fact already owned by the settings catalogue or a format descriptor is not
copied into a second manually maintained fact file. The generator's binding
reads a pure, development-build view of that same immutable definition, or the
rule-generated table becomes that definition's sole owner. It does not link a
full fsimg binary to obtain a catalogue whose construction depends on its own
generated output. The adoption review specifies which direction applies.
Effective settings defaults, native qualification and metadata representability
are not inferred merely from generic format names.

## Binding representation

A binding names each external predicate by name and arity, each column by its
role, and each output by its concrete C destination. Its fixed descriptor kinds
cover the following information:

* An integer column has a concrete C scalar type, inclusive numeric range and
  unit. A symbol column has a closed map from Relic symbol bytes to named C enum
  constants. A boolean is mapped explicitly to the symbols `true` and `false`;
  those are symbols in Relic, not a new language type.
* An input relation is either a compile-time constant table or one optional or
  required tuple projected from the current native input. Optionality names a
  real presence condition. No arbitrary C string is evaluated as an accessor.
* An exported table declares column order, unique key, explicit row order and
  destination member types. An exported local result is a boolean, a bounded
  set of named violations, or a scalar with a proven single answer.
* A diagnostic binding names the existing status, operation, field or setting
  identity, fixed message and its precedence within the owning validation phase.
* Every bound constant, native member and destination has a compile-time type
  check. `sizeof` and `offsetof` are used only inside the generator/native C
  build; their host-dependent values never enter serialized facts or manifests.

For example, the following describes the information in a binding, not a new
configuration syntax or a public C ABI:

```text
profile_max/2
    source: immutable profile table
    column profile: symbol { qnx4, qnx6, ifs }
    column maximum: uint32_t, integer [0, 4294967295], seconds
sample/3
    source: one tuple iff timestamp.valid
    column seconds: int64_t, integer [INT64_MIN, INT64_MAX], UTC-scale seconds
    column nanoseconds: uint32_t, integer [0, 999999999]
    column basis: symbol { utc, known_offset, wall_clock }
loss/0
    result: bool, caller-owned metadata fidelity result
```

The concrete generator API uses typed descriptors for these closed choices.
Bindings contain identifiers and constants rather than arbitrary source snippets;
the backend emits known templates for native member access and enum conversion.
The fsimg generator compiles against the owning private headers so renamed
members, enum changes and width mismatches fail loudly. Handwritten native
adapters remain small named functions, with ordinary unit tests, rather than
callbacks hidden in a generic runtime binding interpreter.

A local input type is specific to its operation. It may borrow the actual
validated record where that avoids copying, or receive individual scalar
arguments. There is no common tagged-tuple ABI between drivers. Public option
padding is never inspected, and pointers, native booleans and enums are never
constructed by casting untrusted bytes. Unrelated native fields need no binding.

## Compilation and specialization

The generator performs these stages before replacing any artifact:

1. Load the explicitly listed sources through Relic's frontend. Apply its syntax,
   safety and stratification checks. Resolve every declared external predicate;
   refuse unbound reads and misspelled outputs even if Relic would merely warn
   about a relation with no definition. A declared empty relation is explicit.
2. Match bindings against predicate arities, constants and every variable use.
   Propagate column types and numeric ranges. Reject ambiguous mixed types,
   duplicate external definitions, conflicting symbol maps and missing outputs.
3. Separate dependencies that are wholly static from dependencies on the local
   input. Reject cycles and unsupported constructs in the selected generation
   subset. A local input cannot influence a compile-time table.
4. Evaluate static relations through Relic. Collect complete successful results,
   validate keys, cardinalities and destination ranges, then sort by declared
   value order. Engine insertion order and interned identifiers do not determine
   emitted row order.
5. Lower dynamic dependencies from checked symbolic rules. Partially evaluate
   static joins, then emit direct comparisons, branches and bounded table
   membership. Produce a bound on input tuples, temporary values, call depth,
   comparisons and stack storage from the accepted plan.
6. Render C, private declarations and the manifest into a temporary directory.
   Validate the complete output set, including ASCII and source provenance,
   before exposing it for review. A failed generation leaves the checked-in set
   untouched; CI compares files without installing them.

The symbolic backend input contains predicate identities as names and arities,
typed columns, literal/expression trees, dependencies and source locations. It
contains no evaluator-owned relation, index, arena or symbol-table pointers.
Relic owns normalization and rejects unsupported shapes with the offending
literal's location and the reason. A structured backend-contract version covers
these meanings; it is not inferred from the evaluator's shared-library SONAME.
The fsimg generator uses an explicitly compatible Relic revision and fails at
startup if that contract is unavailable or differs.

Static generation uses the evaluator's public load/check/query operations and
copies result values before their owner is released. A load, check, evaluation,
resource or output-write failure invalidates the complete generation. A missing
row is never substituted for a failed query. The emitter escapes bytes as C
literals and checks every destination width before rendering constants.

## Closed supported subset

Both local specialization and the static source families start with finite,
acyclic rules. The subset is deliberately smaller than Relic's language:

| Construct | Accepted shape |
| --- | --- |
| Facts | Finite checked-in or bound constant facts, integers and closed symbols |
| Positive literals | Joins over static tables and the current record's bounded input tuples |
| Negation | Relic `not`, over a fully evaluated lower dependency; all tested variables bound |
| Comparisons | `=`, `!=`, `<`, `<=`, `>`, `>=` over compatible bound values; symbol ordering is excluded |
| Assignment | A named variable assigned a constant, bound value or admitted integer expression |
| Integer arithmetic | Unary minus and `+`, `-`, `*`, `/`, `mod`, only when all intermediate operations are proven total on the binding domain |
| Outputs | Fixed tables, boolean existence, closed violation identities, or provably functional scalar results |

Recursion, aggregates, dynamic strings, regular expressions, floating point,
foreign functions, runtime file imports, image-wide inputs and arbitrary user
queries are refused. Static recursion is not accepted simply because a specific
run happens to terminate. Whole-image grouping has its own explicitly bounded
lowering in the generated-verification design; it does not enlarge this local
subset by implication.

Proof is intentionally simple: finite symbol enumeration, propagated intervals,
exact constants and straightforward singleton/optional relation cardinalities.
The backend does not grow an SMT solver or a general C optimizer to accept an
otherwise awkward expression. If interval analysis cannot establish a property,
the rule is refused even if a human could prove it through a complicated
correlation. A direct helper or a clearer relation is preferable.

Optional tuples and fixed tables bound joins independently of image size. A
backend may retain a small fixed loop over a static table; its bound appears in
the manifest. It may not replace unrolling with a heap tuple store or cache.
Repeated derivations have set semantics: boolean existence is idempotent, table
rows are deduplicated before rendering, and a scalar export is refused unless
its key determines exactly one output whenever its documented precondition
holds. An empty scalar result is explicit absence, not zero initialization.

## Numeric, absence and error semantics

Relic integers are signed 64-bit values. Its arithmetic detects overflow,
division by zero, and `INT64_MIN / -1`; `INT64_MIN mod -1` also overflows.
Division truncates toward zero and remainder has the dividend's sign.
Generated operations preserve these meanings. They do not use C signed
overflow, unsigned wraparound, saturation or floating conversion as substitutes.

An fsimg `uint32_t` fits directly. An fsimg `uint64_t` binds as an integer only
when its full admitted domain is independently at most `INT64_MAX`. An image
format's actual width or an existing semantic restriction can establish that
fact; convenience for the generator cannot. Values are checked before casting.
A type's narrower declared range is established by the adapter before use and
has a documented native failure path for values outside it.

Ranges cover every value the generated operation is responsible for classifying,
including corrupt and unsupported encodings. They cannot assume the conclusion
of the predicate being generated. An earlier native range check is admissible
only when that stage already owns the rejection and its status and precedence;
it is not a new filter inserted merely to make a proof succeed.

Full-range offsets, lengths, identifiers and counters remain `uint64_t` in C.
They stay outside local numeric rules, or enter as a completed boolean result
of an existing checked native operation. For example, the bounded-I/O owner
checks `offset <= length && count <= length - offset` in its native unsigned
domain; a rule does not reimplement that check in signed arithmetic. Such an
adapter is evidence to test independently, not a new arbitrary-call feature in
Relic. Splitting values into words, encoding them as decimal strings or treating
them as signed bit patterns is not an implicit extension of the language.
The whole-image backend's rule columns have the same signed-range requirement;
wider unsigned values remain in native C operations outside rule numeric
lowering.

There is no global `INT64_MAX` limit on legal fsimg images. An operation whose
valid domain needs wider arithmetic remains native until a separately designed
and reviewed semantic extension supplies it. Generation refusal is a developer
failure, not permission to add an image rejection or runtime fallback.

Presence has three distinct situations:

* **Present and decoded:** emit the typed tuple, after framing and structural
  validation. In-range semantic values may still violate a rule.
* **Absent by contract:** emit no optional tuple, or the family's explicit
  presence fact. Zero, false and an explicitly empty value are not absence.
* **Malformed, unreadable or unexamined:** do not invoke the predicate as if the
  tuple were absent. Return the decoder's failure or finish the required input
  acquisition first.

No validity bit makes an uninitialized field readable. An adapter branches on
presence before loading optional fields. A type mismatch in general Relic can
make a comparison have no value; the typed generation subset refuses that
possibility instead of using it as a silent filter. Unknown native enum values
are validated by the owner, not mapped to a benign default symbol.

Potential arithmetic errors require particular care because Relic evaluation
strategies can encounter different bindings before failing. The production
local subset eliminates that ambiguity by proving every admitted expression
total over its complete binding domain, without relying on body-literal order
or a short-circuit guard. For instance, `N != 0, Q = V / N` alone is not a
proof under reorderable evaluation. The input relation must have a proven
nonzero divisor domain or the division stays in a native checked helper.

A static evaluator error fails generation with its source location and class.
A runtime failure from a native prerequisite propagates its existing status;
it is never a false predicate or a successful empty result. A violated internal
binding invariant is a contract failure, not an image verdict. Admission tests
establish that arbitrary image bytes cannot reach that failure by bypassing
the adapter. Image-caused malformed values retain `FSIMG_ECORRUPT`, unsupported
features retain `FSIMG_EUNSUPPORTED`, and option errors retain `FSIMG_EINVAL`
as their owning contracts specify.

## Worked example: whole-second timestamp fidelity

The [metadata loss contract](design-metadata-loss.md#unsigned-whole-second-selection)
defines a shared QNX4/QNX6/ImageFS predicate: a present timestamp is preserved
exactly only when its seconds fit the profile, its basis is UTC, its fraction
is zero, and any reproducible substitution leaves its value unchanged.
Selection of the fallback, encoding and loss reporting remain native operations.
This example derives fidelity only, without acquiring a second timestamp policy.

```prolog
profile_max(qnx4, 2147483647).
profile_max(qnx6, 4294967295).
profile_max(ifs, 4294967295).

in_range :-
    selected_profile(P), profile_max(P, M), sample(S, _, _),
    S >= 0, S <= M.

unchanged :- reproducible(false).
unchanged :- reproducible(true), sample(S, _, _), fallback(S).

exact :- in_range, sample(_, 0, utc), unchanged.
loss :- sample(_, _, _), not exact.
```

Each invocation supplies one `selected_profile/1`, one `reproducible/1`, and one
validated `fallback/1`. It supplies zero or one `sample/3`. The adapter validates
nanoseconds and basis according to the metadata contract and checks the fallback
against the selected profile before this calculation. Signed seconds span all
of `int64_t`; the rules compare them without arithmetic. Known-offset seconds
remain the already normalized UTC instant; `known_offset` still fails fidelity
because these profiles preserve only the UTC basis.

Static evaluation produces the profile/maximum table in declared profile order.
Local specialization produces ordinary boolean tests with the profile maximum
already bound by the owner. Its private interface can be this narrow:

```c
bool fsimg_generated_seconds_loss(bool present, int64_t seconds,
    uint32_t nanoseconds, fsimg_time_basis_t basis, uint32_t maximum,
    uint32_t fallback, bool reproducible);
```

The basis type is the owning timestamp header's actual enum, not a guessed
integer representation. `FSIMG_TIME_PER_TIMESTAMP` is a volume summary value
and is not an admitted basis for an individual timestamp.
The adapter does not evaluate absent timestamp members to construct these
arguments: it returns no loss immediately for absence, or passes initialized
neutral arguments. The generated function tests `present` before semantic use.
`maximum` and `fallback` are checked against the selected table by the owner;
passing an arbitrary maximum does not choose a new profile.

An absent timestamp yields no loss. A UTC, zero-fraction timestamp at
2147483647 fits QNX4; 2147483648 does not, but fits QNX6 and ImageFS. A known-
offset timestamp at either valid value is inexact. In reproducible mode, a
valid UTC whole-second timestamp is exact only when equal to the fallback
(epoch). Both `INT64_MIN` and `INT64_MAX` are legitimate inputs to this fidelity
calculation and yield loss, with no arithmetic error.

The owner encodes the selected scalar and submits one timestamp bit to the
existing writer loss operation when `loss` holds. The predicate never counts,
prints, invokes a callback or chooses whether loss is permitted. A required
field's refusal still occurs at the same point with the same path. This small
example explains the binding; adoption still requires evidence that generating
this family removes more maintenance obligations than a direct shared helper.

## Deterministic diagnostics

Violation outputs identify constraints, not prose assembled from untrusted
values. Bindings give each violation an explicit rank within its existing
validation phase. On one record the generated validator computes applicable
violations and selects the smallest rank, with a declared field/key order when
a constraint has several witnesses. Equal ranks with no total tie-break are a
generation error. A scalar boolean fidelity result needs no diagnostic ranking.

A rule order, join plan, table insertion order, compiler optimization or Relic
thread count cannot choose the reported violation. Local validation does not
reorder acquisition failures or neighboring native checks: generation preserves
the caller's phase order, including settings catalogue order, CLI argument
order and unsupported-profile precedence. A migration that changes that order
requires an explicit contract decision and independent tests.

The local function returns a named violation or fills a fixed-size result. Its
native owner attaches the existing `fsimg_status_t`, operation, path and bounded
message through the normal error interface. Runtime diagnostics contain no
absolute generator source path, scratch filename, heap address, source contents
or rule-engine internals. Source file/line mappings are retained in build
artifacts for developers. They are escaped and repository relative.

Selection occurs only after all required computations for that result succeed.
An I/O, resource, cancellation or evaluation failure cannot be replaced by a
previously observed violation or a partial answer. For whole-image operations,
[generated verification](design-generated-verification.md) specifies witness
ordering, completion and the execution-failure boundary in detail.

## Runtime bounds and security

A local generated function has no allocator, mutable global, engine context,
interpreter loop, symbol interner, fact insertion, runtime query, I/O or callback.
It uses scalar temporaries, fixed tables and statically bounded control flow.
Its call depth and stack bound are derived at generation, included in the
manifest and reviewed alongside the handwritten caller's existing stack use.
No image field controls an allocation size or loop bound in this local subset.
Large bounded arrays are not a way to hide a relation engine on the stack.

Cancellation and cumulative work accounting stay at the caller's record loop;
a tiny finite predicate does not create its own polling protocol. If the plan
is too large to fit that loop's existing interruption interval, generation is
refused for local use or the operation is designed as an explicit bounded
whole-image step. Resource limits never trigger fallback to a resident engine.

Byte decoding remains in its format owner, using the
[scalar primitives](design-binary-primitives.md) only after bounds checks.
Rules receive decoded values, not raw pointers into images. They cannot open a
source, authorize extraction, invoke a host command, modify a sandbox or publish
an output. Failing predicates leave caller-owned output untouched unless the
private interface explicitly defines a fixed initialized failure result.

Generation is a developer operation over repository sources. It runs offline
with explicit paths and bounded process resources. Relic's evaluator memory
limit is useful but is not a total process-memory or time limit: source loading,
compiler work and static evaluation also receive CI process/time ceilings.
Exceeding either ceiling fails generation, not a release build. The generator
opens no paths named by facts, invokes no source-supplied commands, and escapes
all emitted identifiers and strings through their declared closed mappings.

Generated and handwritten project code carries BSD-2-Clause notices. Review
provenance per emitted support file: using a BSD-licensed engine does not prove
every imported template or test oracle can be copied. If support arithmetic is
copied from Relic, preserve its copyright and license notices and record that
source in the manifest; prefer fsimg-owned existing checked operations when
their semantics match. There is no copied evaluator or unreviewed amalgamation.

## Reproducible development and releases

The generator lock records an exact Relic source revision, backend-contract
version, generator source revision or digest, and its build requirements.
Regeneration checks compatibility explicitly before reading rule outputs.
A different compatible version is not silently selected from `PATH`; updating
the pin is a reviewed change with the differential suite. Developers install
the required development tools or report them missing; ordinary users need
none of them.

For each family the manifest records sorted input names and SHA-256 digests,
the lock identity, binding/schema version, exported symbols, source mappings,
static table cardinalities, numeric assumptions, bounded operations and output
digests. The manifest excludes its own digest to avoid self-reference. Digests
cover file bytes, not mtimes. Source/binding versions identify the generation
contract, not a new image or wire version. Absolute paths, timestamps, locale,
engine domain numbers, native structure sizes and host build paths are absent.

`make regen-rules` explicitly regenerates the declared families with the locked
tool into a temporary output directory and validates the complete set before
applying it. It replaces exactly each declared family's `.c`, `.h` and
`.manifest` artifacts at the paths listed above, deletes no unrelated files,
and returns success only after every replacement succeeds. An interrupted
application is an ordinary incomplete working-tree edit: manifest checks and
CI detect it, and rerunning generation or restoring the affected files repairs
it. No multi-file atomicity or separate publication protocol is required.
`make check-generated` regenerates into temporary output
and compares every declared artifact byte for byte; it fails for missing, stale
or extra family outputs. These are development targets. Neither `configure`,
ordinary `make`, `make check` nor a release-tarball build implicitly regenerates.

A release contains rule sources, binding sources, the lock and all generated
C/header/manifest artifacts. The release gate runs reproducibility checks with
the pinned generator, then builds from the distributed artifacts in an
environment without Relic. Failure to reproduce blocks release; it does not
cause a consumer's build to download a tool. A missing generated source is an
incomplete source distribution and yields a direct build error.

Generated declarations include a common family identity checked against their
implementation at compile time where practical. CI's complete-file comparison
and manifest hashes check the whole family; they do not depend on a header
constant alone to detect a stale source. Generated C is reviewed for readable
names, typed data flow, initialized outputs, immediate failure checks and source
attribution. Large opaque boolean expressions or numeric instruction arrays
fail that review even when deterministic.

## Evidence and adoption gate

Three independent kinds of evidence are required:

1. **Language and lowering equivalence.** Small typed fixture inputs run through
   the Relic evaluator and generated C. Compare complete answer sets, absence,
   diagnostics selected by the binding contract, and admitted integer endpoints.
   Refusal tests cover recursion, aggregates, wrong arities, type conflicts,
   ambiguous scalar outputs, unknown symbols and arithmetic not proven total.
   The evaluator is a lowering oracle, not an oracle for filesystem correctness.
2. **Domain correctness.** Hand-authored boundary cases and malformed images
   derive expected outcomes from the format specification and existing public
   contracts. External implementation evidence and independent codecs check
   actual image bytes. Test expected values are not emitted from the production
   rules, profile facts or generated tables. Reusing adapters between both sides
   cannot establish that the adapter interprets a field correctly.
3. **Operational compatibility.** Existing driver contract tests check option
   defaults, capability membership, exact status/precedence, source consistency,
   metadata loss events, reproducible output and cleanup. Build/link inspection
   verifies no Relic runtime symbols or libraries; a clean release build without
   the generator proves the ordinary dependency boundary.

Targeted tests exercise absent fields with inaccessible or poisoned unused
storage; present zero and false; invalid enum and nanosecond inputs; duplicate
static facts; ties between violations; maximum admitted ranges; and values above
`INT64_MAX` through the native unsigned paths. They verify output remains
unchanged on prerequisite failure. Generator fault injection covers allocation,
write and evaluator failures before application, leaving tracked artifacts
unchanged. Application failures return failure, identify the affected artifact,
and are detected by regeneration comparison; no incomplete set is reported as
success.
Changing locale, working directory, source path prefix, supported host and
Relic worker count yields identical artifacts.

The coordinating implementation session runs the project's required integrated
sanitizers, platform checks and reviews. Generated C receives the same security
and readability review as handwritten C. Generator differential tests supplement
rather than replace driver fixtures, fuzzing and independent image validation.
Skipped tools or unavailable platforms are reported explicitly.

A family is adopted only when a before/after review counts the rules, bindings,
generator changes, support code, generated artifacts and tests together. It must
remove duplicated logical ownership or repeated derivation, preserve the
existing diagnostic and resource contracts, and be understandable from the
sources and emitted C alone. A second format must reuse the same bounded
mechanism without format-name branches in the Relic backend. An experiment
that merely moves a clear helper into several files stays an experiment;
a plain shared helper remains the right implementation in that case.
