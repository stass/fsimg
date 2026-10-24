# Relic results and C templates

## Purpose and ownership

Relic evaluates trusted, finite filesystem metadata at development time. Its
ordinary query output describes static tables and instances of known C
validation templates. A small POSIX AWK renderer turns those rows into readable,
checked-in C11. This baseline needs no changes to Relic: its ordinary CLI
supplies the complete result boundary. Neither the library nor the CLI loads
rules, invokes Relic, or maintains a runtime fact database.

This design is useful when relationships between profile facts derive several
otherwise repeated tables or choose the same validation shape for several
formats. It does not compile arbitrary Relic predicates into C. A rule can
compute which profile needs a whole-second timestamp test; it does not compute
the answer for a timestamp read from a future image. The renderer supplies that
runtime test from a reviewed, finite template.

Handwritten C owns decoding, presence, traversal, checked arithmetic, bounded
I/O, scratch storage, cancellation, and failure policy. Generated code supplies
immutable tables or small pure predicates within those existing operations.
The [architecture contracts](architecture.md#contracts),
[metadata loss contract](design-metadata-loss.md), and
[settings contract](design-settings.md) retain authority over their semantics.
[Generated verification](design-generated-verification.md) applies the same
result-row mechanism to known verification predicates; its runtime algorithms
remain handwritten.

This pipeline needs no Relic backend, AST export, static type inference,
compiler IR, expression language, or fsimg parser for rule source. Only Relic reads `.dl`.
If the AWK renderer starts planning joins, interpreting expressions, or selecting
image traversal algorithms, the change exceeds this design. Keep that operation
in ordinary C and reconsider whether generation buys anything.

## Relic extension boundary

A concrete need may justify extending Relic when the extension also serves
other consumers. Its `specs/architecture.md` principles favor one relational
engine, conventional Datalog semantics, inspectable execution, deterministic
results, and transformations that avoid complicating the evaluator. A proposal
must identify the general workload and the obligations it removes, and expose
an ordinary public library or CLI boundary useful outside fsimg. Compare the
whole maintenance model rather than moving fsimg complexity behind that API.

Such a change is specified, independently tested, and reviewed in Relic. It
preserves Relic's semantics and determinism. fsimg-specific format identities,
validation templates, storage algorithms, and callbacks stay outside the engine.
The existing result-rendering path requires no such extension; extensions are not
prerequisites or a speculative feature list. Any adopted extension remains a
development dependency and does not change fsimg's runtime dependency boundary.

## Sources and result contract

The complete input list is explicit in the generation script. Facts have one
authoritative home: either the rules own a definition and its C table is
generated, or an existing immutable catalogue supplies development-time facts.
Adoption specifies that direction; it does not copy a catalogue into a second
manually maintained list or link a generator against its own generated output.

| Artifact | Responsibility |
| --- | --- |
| `specs/rules/*.dl` | Ordinary Relic metadata facts and derivations, with format-source citations |
| `tools/relic/<family>.awk` | Closed row validation, constant mappings, and a small finite set of C templates |
| `tools/relic/generate.sh` | Explicit input/output paths, tool identity, staging, exit-status checks, and application |
| `tools/relic/toolchain` | Exact Relic source revision and its development build requirements |
| `lib/<owner>/generated/*.h` and `.c` | Private, checked-in C compiled by ordinary builds |
| `tools/relic/generated.manifest` | Input and renderer hashes, tool identity, schema version, and output hashes |

Rules use Relic `%` comments for copyright and SPDX BSD-2-Clause notices.
Scripts and emitted C carry the same project license. Using Relic's output
does not require copying its implementation; any imported support code still
requires an individual provenance and license review.

Each family has one explicit result relation and one query, with named columns
for its own data. It has a small dedicated AWK renderer; families share the
transport and staging contract, not a generic template language. The timestamp
family exports:

```prolog
timestamp_output(Kind, Profile, Template, Minimum, Maximum, Basis)?
```

Its six columns are a closed, tagged row schema. They are not arbitrary code,
member accessors, paths, expressions, or diagnostic prose. Their wire values
are ASCII tokens; each template defines the semantic type of its arguments.
The initial rows are:

| Kind | Profile | Template | Minimum | Maximum | Basis |
| --- | --- | --- | --- | --- | --- |
| `table` | `ifs`, `qnx4`, `qnx6` | `unsigned_seconds` | `zero` | `i32_max`, `u32_max` | `utc` |
| `predicate` | `ifs`, `qnx4`, `qnx6` | `whole_seconds` | `zero` | `i32_max`, `u32_max` | `utc` |

The verification family defines its own named columns in
[generated verification](design-generated-verification.md). Each timestamp
profile maps explicitly to its C name and owning header. Adding a template or
identifier changes this family's renderer, contract, and tests together. There
is no fallback for an unknown template or token.

The renderer requires exactly six nonempty tab-separated fields, rejects blank
lines, unknown tokens, escapes, controls, and extra columns, and validates the
allowed combinations for each template. It checks strict canonical row order,
uniqueness of `(Kind, Profile)`, and the complete required key set. Timestamp
profiles have exactly one table and one predicate row, with matching arguments. A
conflicting profile definition cannot silently pick whichever answer arrives
first. An empty result or a missing final profile fails generation. Identical
Relic facts have set semantics and collapse before output; uniqueness here is
about the resulting definitions, not repeated source spelling.

Required keys and token-to-C mappings are the small explicit integration
boundary, not a second copy of every derived answer. For example, the renderer
knows which C profile names exist and which constants fit a template's type;
the rules derive which allowed maximum belongs to a profile.

## Ordinary CLI transport

The generation script invokes the pinned normal CLI, for example:

```sh
LC_ALL=C relic -s -F tsv \
    -q 'timestamp_output(Kind, Profile, Template, Minimum, Maximum, Basis)?' \
    specs/rules/profiles.dl specs/rules/timestamps.dl > "$tmp/rows.tsv"
```

The script checks that command's exit status before invoking AWK on the staged
file. It does not use a shell pipeline whose final AWK status could hide a
Relic failure. It supplies exactly one `-q`; program queries do not add output
when explicit queries are supplied. Diagnostics remain on stderr.

This uses the output documented in Relic's `man/relic.1` and
`specs/design-cli.md`, implemented in `cmd/relic/output.c`: TSV has no header,
columns follow the first occurrence of query variables, and `-s` sorts answers
lexicographically by columns. Rules emit only symbols, so this is bytewise token
order. With `LC_ALL=C`, AWK checks that order as strings, including the complete
row rather than comparing numeric-looking arguments.

TSV does not preserve the distinction between a symbol and a string with the
same bytes. The schema types the resulting tokens, not Relic's internal value
tags; that distinction has no meaning at this boundary. The allowlists reject
all other byte spellings. TSV escapes backslash, tab, newline, and carriage
return; this schema needs none of them and rejects them rather than growing a
general escape decoder. Human-readable explain output and engine-private data
are never inputs.

## Worked example: a derived static table

Profile facts describe an encoding and its required time basis. Encoding facts
own the corresponding bounds. The join derives each profile's table entry:

```prolog
% SPDX-License-Identifier: BSD-2-Clause
profile_time(qnx4, seconds31, utc).
profile_time(qnx6, seconds32, utc).
profile_time(ifs, seconds32, utc).
encoding_bounds(seconds31, zero, i32_max).
encoding_bounds(seconds32, zero, u32_max).

timestamp_output(table, P, unsigned_seconds, Lo, Hi, B) :-
    profile_time(P, E, B), encoding_bounds(E, Lo, Hi).
```

The table rows, displayed here with spaces for readability, are:

```text
table ifs unsigned_seconds zero u32_max utc
table qnx4 unsigned_seconds zero i32_max utc
table qnx6 unsigned_seconds zero u32_max utc
```

The actual transport uses tabs. The table template emits a private array using
the owning profile identifiers and a `uint32_t` maximum member, for example:

```c
[FSIMG_PROFILE_QNX4] = { .maximum = UINT32_C(2147483647) },
[FSIMG_PROFILE_QNX6] = { .maximum = UINT32_C(4294967295) },
```

These C names illustrate the binding; an adopted family uses its owner's actual
private enum and type, with no new public profile API. Basis and lower bound
select the supported table shape; the renderer rejects combinations that shape
cannot represent. Several profiles share the encoding facts, so changing an
encoding bound has one source rather than one edit per derived profile table.
A table already expressed clearly by a few direct C initializers does not need
this machinery.

## Worked example: a runtime fidelity predicate

The same metadata selects a known predicate template:

```prolog
whole_second_encoding(seconds31).
whole_second_encoding(seconds32).

timestamp_output(predicate, P, whole_seconds, Lo, Hi, utc) :-
    profile_time(P, E, utc), encoding_bounds(E, Lo, Hi),
    whole_second_encoding(E).
```

For QNX4 this derives the row:

```text
predicate qnx4 whole_seconds zero i32_max utc
```

This is a request to instantiate `whole_seconds` with a maximum of 2147483647,
not a computed fidelity answer. The template implements the existing
[whole-second contract](design-metadata-loss.md#unsigned-whole-second-selection).
Its QNX4 instance has this shape:

```c
static bool
qnx4_seconds_exact(const fsimg_timestamp_t *time, uint32_t fallback,
    bool reproducible)
{
	if (time == NULL)
		return (true);
	if (time->seconds < 0 || time->seconds > INT64_C(2147483647))
		return (false);
	if (time->nanoseconds != 0 || time->basis != FSIMG_TIME_UTC)
		return (false);
	if (reproducible && time->seconds != (int64_t)fallback)
		return (false);
	return (true);
}
```

The native caller supplies `NULL` for an absent timestamp and does not read its
unused members. For a present value it supplies initialized, validated metadata;
invalid enum or fraction encodings retain the decoder/catalogue's failure path.
The caller validates fallback against the selected profile, chooses the encoded
value, and reports a loss only for a present field whose predicate is false.
The template neither counts losses nor samples a clock nor chooses whether loss
is permitted. It cannot change validation phase or diagnostic precedence.

Seconds span the full `int64_t` domain. Negative seconds and values above the
profile maximum return false without arithmetic or narrowing. QNX4 accepts
2147483647 and rejects 2147483648; QNX6 and ImageFS accept the latter. Nonzero
fractions and non-UTC bases are inexact. Reproducible substitution is exact only
when the original whole-second UTC value equals the validated fallback. An
absent timestamp incurs no loss.

The C expression is authored in the template, not recovered from a rule body.
A new runtime condition requires a reviewed template change or stays in a
handwritten helper. Merely moving the existing shared whole-second helper into
rules and templates is not an improvement: this example establishes feasibility,
while adoption requires reduced duplication across the complete family.

## Exact constants and bounded execution

AWK never converts a semantic constant to a number. The initial schema uses
closed symbolic constants. `i32_max` maps to a literal string such as
`UINT32_C(2147483647)` for a table member or `INT64_C(2147483647)` for a signed
comparison. `u32_max` similarly maps to 4294967295 in the template's C type.
A template that genuinely needs full unsigned width can admit `u64_max` and
map it directly to `UINT64_MAX`; no Relic signed integer or AWK floating-point
value needs to represent 18446744073709551615.

There is no `+ 0`, numeric `sprintf`, numeric comparison, or arithmetic on these
constants in AWK. Unknown constants fail rather than being copied into C.
This keeps the renderer small and exact, including values above 2^53 and
`INT64_MAX`. Runtime values remain in their native C types; no legal image is
rejected to fit Relic's signed numeric domain. Numeric calculations on
image-sized offsets remain in the existing checked C operations.

Local generated predicates use fixed scalar work, no allocation, mutable global
state, I/O, callbacks, or image-dependent loop. The caller retains resource
accounting and cancellation at its existing record loop. Neither rows nor
rules authorize reading bytes, naming host paths, changing confinement, or
publishing an output. Generated diagnostics use existing native identifiers;
no source paths or scratch filenames enter product diagnostics.

## Generation and release workflow

Generation runs offline over repository files, under development/CI time and
process-memory limits. Relic's evaluator limit alone is not a whole-process
limit. The small finite result is staged in a private temporary directory. The
script checks Relic success, runs AWK validation/rendering, checks AWK success,
and validates the complete declared artifact set before replacing tracked
files. A malformed row, missing key, failed evaluation, or failed staged write
leaves tracked artifacts untouched. AWK may stream text to staging; that partial
text is never a candidate for application after failure.

`make regen-rules` applies only the declared generated files after those checks.
Replacement failures are failures, with the affected path reported. Interruption
during several replacements can leave an ordinary incomplete working-tree edit;
rerunning generation repairs it. There is no second publication coordinator or
claim of multi-file atomicity. `make check-generated` regenerates in temporary
storage and compares the complete declared set byte for byte without applying it.

The toolchain file pins the exact Relic source revision; regeneration checks the
supplied executable's provenance against that development build, rather than
accepting an arbitrary `PATH` version. The small manifest records that identity,
the row-schema version, sorted repository-relative input paths and SHA-256
hashes, renderer/script hashes, and generated C/header hashes. It excludes its
own hash. Output contains no timestamps, absolute paths, native addresses, or
engine-internal identifiers. There is no per-function proof manifest or separate
compiler compatibility API.

Ordinary `configure`, `make`, and `make check` consume distributed generated C
and never invoke Relic. Releases include rules, templates, generation scripts,
toolchain identity, manifest, and generated sources. The release gate checks
reproduction with the pinned development tools and builds the distribution
without Relic installed. A missing generated source is a source-distribution
error; it does not cause a download or hidden regeneration.

## Evidence and adoption

Test the actual CLI-to-AWK boundary with finite metadata fixtures and independently
written expected rows. Renderer tests cover wrong field counts, unknown tags or
constants, conflicting keys, reordered/duplicate rows, missing required rows,
escapes, and empty/truncated output. Simulate a Relic process that writes a valid
prefix then fails, an AWK failure, and staging write failures; none may replace
tracked artifacts. Verify symbolic 64-bit endpoints emit exact typed C tokens.

Compile the generated C and test runtime boundaries independently of the
production facts and renderer. Timestamp tests include absence with inaccessible
unused storage, zero, profile maxima and their neighbors, both signed endpoints,
all time bases, fractions, and reproducible fallback equality. Existing driver
fixtures check loss counts, required-field refusal, status precedence and bytes.
Expected format answers come from the format contracts and independent image
tools, not from the same generated table being tested.

Relic evaluation of sample inputs can supplement those tests if a separate
reference predicate is useful; it does not prove a general compiler equivalence
because no general compiler exists here. Reproducibility checks vary locale,
working path, supported host, and Relic thread count. Ordinary build and link
checks establish the absence of a Relic runtime dependency. Generated C receives
the same readability, security, sanitizer, and platform checks as handwritten C.

Adoption compares the complete maintenance model: metadata facts and rules,
closed row contract, renderer templates, generated code, native callers, and
tests. It must remove repeated ownership or derivation, not merely redistribute
a simple helper across more files. This boundary is viable for static tables
and a few repeated validation shapes. Arbitrary predicates and whole-image
algorithm compilation are outside its scope.
