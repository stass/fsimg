# Generated verification policies

Ordinary Relic queries derive finite, static verification policies from trusted
format descriptions. A small AWK renderer turns their results into known C
predicates. Handwritten C still walks the image, collects references, sorts,
counts groups, merges with inode tables, reports errors and releases resources.
Relic never processes an image during verification. This design removes repeated
policy decisions where that reuse earns its cost; it does not generate a
whole-image verification algorithm.

The [generation design](design-relic-generation.md) owns the shared query-result
transport, ordinary Relic invocation, renderer validation, deterministic output,
provenance, checked-in artifacts and regeneration/build contract. This document
specifies one concrete query family and its fixed renderer within that contract.
This baseline needs no Relic change, backend, intermediate instruction language,
proof engine or generic execution interpreter. AWK recognizes named templates;
it does not parse rules, infer joins, prove cardinality or schedule I/O.

Generic Relic improvements remain eligible under the
[generation design's extension criteria](design-relic-generation.md): they fit
Relic's philosophy and have utility beyond fsimg. This design requires none and
does not depend on a speculative extension. Runtime grouping, scheduling,
resource management and format-specific verification remain native fsimg
responsibilities in this design.

The [verification helpers](design-verification-helpers.md),
[storage scope](design-storage.md#verification-scope),
[prepared directories](design-directories.md), [content](design-content.md),
[API](design-api.md#extraction-and-verification) and
[security](design-security.md) contracts govern runtime execution. Format designs
remain authoritative about identities, counts and complete domains.

## The useful boundary

QNX6 and QNX4 both reconcile names against object records, but their runtime
algorithms and directory policies differ. Their common policy is small:
an unused record has no names, a used record has at least one name, and selected
used records require an exact link count. Deriving that policy can keep multiple
static consumers consistent. It cannot remove the native reconciliation loops.

| Responsibility | Owner |
| --- | --- |
| Describe which object classes compare stored links with observed names | Trusted format facts and ordinary Relic rules |
| Select a known predicate and its finite configuration | Ordinary Relic query result |
| Emit that predicate with fixed C syntax and typed constants | Small AWK renderer |
| Decode state, establish identities, collect occurrences, count and bound them | Native driver |
| Traverse, sort, read windows, group, merge, order faults and finish | Native driver and existing helpers |
| Budget memory/scratch/I/O, cancel, destroy and publish completion | Existing session, metadata and reader lifecycles |

The renderer has no image-derived input, callbacks, expression strings, sort
keys, join clauses or runtime operation lists. A new algorithm remains C. Even
when a static query uses a join to derive a policy row, its output only selects a
known predicate; it does not ask AWK to implement that join over image records.

QNX4's claimed-block bitset and QNX6's owner-bearing allocation runs remain
separate. FAT/exFAT ownership and fused chain walks, ImageFS checksums, long-name
back-references, geometry and namespace checks gain no implementation from this
policy template. Generating them requires a separately justified finite static
artifact, not broadening this template into a verifier language.

## One concrete derivation

The concrete result relation is
`verify_policy(Object, Presence, Links, Basis)`. All four columns are closed
symbol tokens. The format facts derive these expected rows:

| Object | Presence | Links | Basis |
| --- | --- | --- | --- |
| `qnx6_inode` | `unused_zero` | `all_used` | `slot_names` |
| `qnx4_table` | `unused_zero` | `nondirectory` | `slot_names` |

`Object` is the unique row key. Ordinary Relic executes the one query
`verify_policy(Object, Presence, Links, Basis)?` using the shared sorted TSV
transport. The family renderer `tools/relic/verification.awk` accepts only this
relation's fixed shape and renders each row through one predicate template. It has no
generic template registry or artifact instruction language. The shared build
contract stages and checks the complete query output before invoking AWK.

`unused_zero` means unused records require zero references and used records
require positive references. `all_used` selects link equality for every used,
referenced record. `nondirectory` selects it only for non-directories.
`slot_names` means the native occurrence count defined below, not a count of
distinct target IDs or a Relic runtime aggregate. The renderer validates one row
for each of the two named objects, the singleton tokens `unused_zero` and
`slot_names`, and either `all_used` or `nondirectory` for `Links`. Either link
scope is renderable for either object. The renderer does not encode the expected
object-to-scope mapping a second time: independent semantic tests reject an
incorrect derivation. Unknown tokens, duplicate/conflicting keys and missing
coverage fail generation; a missing row never disables a check.

Trusted facts say that QNX6's inode reference domain includes dot
slots, while QNX4's table-name domain excludes dots and checks directory counts
at preparation. A complete example uses ordinary facts and rules:

```prolog
directory_basis(qnx6_inode, dot_slots).
directory_basis(qnx4_table, separate_subcount).
presence(qnx6_inode, unused_zero).
presence(qnx4_table, unused_zero).

verify_policy(Object, Presence, all_used, slot_names) :-
    presence(Object, Presence), directory_basis(Object, dot_slots).
verify_policy(Object, Presence, nondirectory, slot_names) :-
    presence(Object, Presence), directory_basis(Object, separate_subcount).
```

These derivations select only static policy. The facts are reviewed format
knowledge, not extracted observations from an image. Both formats require a
name for every used ordinary table record. The resulting rows and native
diagnostic mappings implement these exact truth tables:

| Used | Has names | QNX6 outcome | QNX4 table outcome |
| --- | --- | --- | --- |
| false | false | Accept; ignore other record bytes | Accept; ignore other record bytes |
| false | true | Link-count error | Free-slot reference error |
| true | false | Unnamed allocated inode error | Unnamed used slot error |
| true | true | Compare links and names | Compare links and names only for a non-directory |

The diagnostic distinction in the second row is deliberate. QNX6's native
check groups an unallocated referenced inode with link-count mismatches; the
generation does not silently change its public failure detail.

One fixed predicate template emits bodies such as the following QNX4 fragment.
The result enum is a private C declaration with four closed outcomes: accept,
free reference, unnamed record and wrong links. It is not an opcode or a
runtime-configurable policy table.

```c
static reference_result_t
qnx4_table_reference_result(bool used, bool directory, uint64_t names,
    uint16_t links)
{
	if (!used)
		return (names == 0 ? REFERENCE_ACCEPT : REFERENCE_FREE);
	if (names == 0)
		return (REFERENCE_UNNAMED);
	if (!directory && links != names)
		return (REFERENCE_WRONG_LINKS);
	return (REFERENCE_ACCEPT);
}
```

The template gives both predicates this same signature and result enum. For
`Links = all_used`, it renders the equality condition as `links != names` and
marks the unused `directory` parameter with the project's `FSIMG_UNUSED`
convention. For `Links = nondirectory`, it renders `!directory && links != names`
as above. The object key selects only the fixed C function name. Both variants
return `REFERENCE_FREE` for an unused record with names; format-specific error
mapping remains native. This is one finite conditional substitution, not an
expression translator. The enum and signatures use project `_t` types, and
emitted source obeys the style guide and BSD-2-Clause/ASCII requirements.

In QNX4, `check_table_slot()` decodes the current slot, treats slot zero as the
signature rather than an object, and derives `used` only for an ordinary slot
with `QNX4_STATUS_USED`. It passes neutral directory/link values for unused slots;
stale bytes have no semantics. It calls the generated predicate at the existing
check location after the native group count succeeds. Its exhaustive switch
maps the result to the existing `qnx4_corrupt()` messages. It does not change
stream advancement, table reads or cleanup.

In QNX6, `verify_inodes()` calls its predicate after the native reference fold,
window read/charge and inode decode. It supplies the allocation bit and the
exact count, with a neutral directory flag and neutral links for an unused inode.
Native C still increments
its referenced-inode total and checks the final free-inode equation. It maps
`REFERENCE_UNNAMED` to its unnamed-inode message and both `REFERENCE_FREE` and
`REFERENCE_WRONG_LINKS` to its existing link-count message. A returned
semantic result is not a library status; operational failures have already
returned through the native path and cannot be converted to false predicates.

The template adds no checks on unused mode or link bytes. Counts remain
`uint64_t`; stored links retain their decoded native width. Neither value passes
through Relic's signed integer domain or AWK's numeric representation. The
renderer never calculates an image count, offset, endpoint or reservation.

## Native domains and occurrence identity

An image's reference stream is an occurrence stream, not a set of inode numbers.
QNX6 stores a four-byte inode number; QNX4 stores a four-byte logical `.inodes`
slot (`REFERENCE_BYTES` in its native verifier). Each driver retains its own
codec and target validation. Policy generation neither selects nor changes the
encoding. Equal targets are retained: two names of one inode contribute two
references. The implicit occurrence
identity is the source directory and logical slot within the volume, with the
selected snapshot additionally fixing QNX6's domain. Exactly-once collection is
a native directory-preparation invariant; no generator proves it. Replaying a
slot is a producer bug, not another legitimate name. Names and path bytes are
unneeded for reconciliation and are not added to the records.

QNX6 collects every live validated slot, including `.` and `..`, before public
name omission. Both root dot slots name the root and remain separate occurrences.
The inode table scan covers `1..inode_count`, including unreferenced/free slots.
The selected snapshot fixes allocation ownership and the free-inode header;
references from other snapshots never mix with it. Repeated hard-link names
increase counts without repeating the object's claims or payload reads.

QNX4 accounts every used ordinary directory slot after the two dot records,
including omitted names. Links to `.inodes` append their logical target slot;
direct and root-block objects are checked at discovery instead. Non-directory
direct/root objects require one name. Directory preparation checks links against
`2 + subdirectories`, including root's `lost+found`; the empty `lost+found`
directory's own count is checked separately because it was read at open.
`.inodes` slot zero is a signature, never an ordinary object or valid reference.
A used table directory needs a name, but its table-name count is not its stored
link count. Free-slot bytes other than defined state are ignored.

Omitted directories are descended under the full verification walk. Unsupported
scope, malformed slots and failed parsing end verification rather than removing
facts. QNX4 invalid-label and nonempty `lost+found` failures retain their
pre-traversal position. A complete empty optional domain differs from an
interrupted producer; only successful native collection proceeds to seal and
sort. The predicate neither establishes traversal completeness nor owns a
second completeness flag.

## Native scheduling and failure precedence

A predicate substitution preserves each existing schedule in
`lib/formats/qnx6/verify.c` and `lib/formats/qnx4/verify.c`. A shared merge engine
would need additional scheduling choices without removing a substantial
algorithm, and is outside this design.

* QNX6 obtains initial reference lookahead, then folds the current inode's group
  and reads its next lookahead **before** loading and charging a new inode
  window. It then decodes and checks that inode. A group/lookahead error can
  precede a current-window read error; an earlier semantic failure precedes an
  I/O failure in a later window.
* QNX4 obtains initial reference lookahead, then reads and charges each table
  window **before** folding the groups of its slots. Each group reads the next
  lookahead before checking the slot. A current-window failure can precede a
  group error within that window; a malformed lookahead can precede the current
  slot's semantic contradiction.
* Each scan covers its full table domain, including groups with count zero,
  and rejects residual out-of-range references. QNX6 then checks
  `inode_count - referenced == free_inodes`. QNX4 continues through long-name
  occupancy and allocation reconciliation; QNX6 retains its allocation checks
  before reference reconciliation. Final source stability and common report
  publication remain at their native boundaries.

No eager validation pass is added to choose a globally preferred error. Sort,
framing and cursor errors retain existing attribution. Semantic errors retain
format-specific diagnostics without raw names in messages. Failure to allocate,
read, write, cancel, maintain source stability or satisfy a budget returns the
existing operational status and leaves an incomplete report.

## Resources and lifecycle

No new runtime object, scratch stream, session, descriptor or lookup database is
introduced by the predicate. The driver retains ownership of source and sorted
reference streams, bitsets, payload buffers and error cleanup. Existing bounded
metadata sort and its simultaneous store reservations remain authoritative.
There is no per-image Relic execution or native fallback to a resident fact set.

Native code owns count bounds, checked arithmetic, table geometry, window sizes,
encoded lengths and sentinel validation. Multiplicity is not deduplicated.
Counts remain exact; report statistics do not replace reconciliation counts.
Existing traversal/storage charging and cancellation boundaries remain in force;
a generated predicate adds constant work only. Any unrelated defect in count
bounds or cancellation needs a native fix and its own regression evidence, not
a claim that static generation proves it safe.

Artifact generation processes trusted development facts only. A failed query,
invalid row set or renderer failure cannot replace a checked-in artifact with
partial output. The single generation contract handles tool availability and
atomic publication; runtime verification works from ordinary compiled C.

## Acceptance and independent evidence

Pipeline tests establish that the tools implement the declared static contract;
format tests establish that the contract is correct. Neither substitutes for the
other, and generating tests and production code from the same facts is not
independent evidence.

Pipeline tests cover both exact rows, rejected unknown objects/columns/tokens,
missing/conflicting/duplicate rows, shuffled result order, Relic failure after
partial stdout, renderer failure and deterministic regeneration. They compile
emitted C and compare every finite state to an independently written truth
table, including directory and non-directory cases, counts zero/one/many and
stored-link boundaries. A schema-valid mutation exchanging the two link scopes
must render successfully and fail the independent expected-policy tests; this
checks that AWK does not duplicate the format policy. Tests of native wide
counts include values above
`INT64_MAX` without routing them through the generator. Query-derived expected
results test transport consistency only.

Driver acceptance retains `test/qnx4.c`, `test/qnx6.c`, their writer/oracle and
integration coverage and the verification-helper contracts. Independent fixtures
and mutations exercise:

* QNX6 dots and root self-references, hard links, wrong directory/file counts,
  unused referenced and unnamed allocated inodes, wrong free totals, both byte
  orders, selected-snapshot isolation and legal cross-snapshot sharing.
* QNX4 table versus direct/root identities, signature slot, free/used table
  slots, table directories with stored links unlike table references, directory
  subcounts, empty `lost+found`, invalid labels and long-name occupancy.
* Omitted names and descendants, repeated targets from distinct slots,
  exactly-once preparation, empty reference streams, zero-count groups, maximum
  target keys and residual references beyond the table.
* Deliberately combined faults around the first and later windows, group-end
  lookahead, table decode and final checks. Assertions name the returned status
  and diagnostic, establishing the distinct QNX4/QNX6 precedence above.
* Allocation, scratch I/O, source-change, cancellation and limit failures during
  collection, sorting and reconciliation. Cleanup returns reservations to
  baseline, and no partial domain publishes complete verification.

For identical images and limits, compare native and substituted versions for
verdict, diagnostic, report fields, payload-read decisions, image reads, stream
record widths/counts, sort passes, work, peak resident/scratch usage and open
descriptors. Forced spill changes no healthy-image verdict or semantic check
order. Differential agreement is a regression check, supplemented by the
independent expected outcomes above. Runtime changes follow the full test,
sanitizer and platform requirements in [AGENTS.md](../AGENTS.md) and
[build instructions](../docs/build.md).

## Adoption criterion

This mechanism can serve both QNX formats only at their small policy boundary.
It removes repeated selection of the free/used/name/link rules when those rules
also drive useful independent static artifacts or documentation. It removes no
reference codec, occurrence producer, grouping loop, merge, window cache,
allocation proof or lifecycle obligation. Independent tests deliberately retain
their own expected semantics.

The before/after comparison therefore counts the new fact declarations, query,
row contract, AWK templates, artifact checks and native call/result mapping
against the policy duplication actually removed. The existing native checks
are short. If the predicates have no useful second consumer, or a direct C
table or two explicit branches convey the policy more simply, the driver keeps
its handwritten check. Adoption requires demonstrated total maintenance
reduction for each format, unchanged runtime bounds and the acceptance evidence
above. These existing per-record conditions alone do not establish enough
benefit to repay the Datalog facts, renderer and artifact maintenance. The
approach works for shared static policy; it does not generate either whole-image
algorithm. Generating a few branches alone is not a reason to add a toolchain.
