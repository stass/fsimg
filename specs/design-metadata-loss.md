# Writer metadata loss and timestamp conversion

## Scope and ownership

The writer owns the policy for metadata it cannot preserve: refusal, the
operation's count, and delivery to the context's loss reporter. Drivers decide
what their formats represent and which replacement bytes they encode. The
[metadata model](design-metadata.md#metadata-fields) owns validity, required
fields and field meanings; the [API](design-api.md) owns public callbacks and
statistics. Extraction retains its separate policy and uses the same context
reporting primitive without acquiring writer state.

A single internal writer operation accepts the writer, a borrowed catalog
entry, a mask of lost fields, a diagnostic explanation and an error output.
It needs no completed format plan, so the common capability check and the
format's planning pass use the same operation. The entry and explanation are
borrowed for the call; no path or event is retained.

For a nonempty mask the operation:

1. Refuses with `FSIMG_EUNSUPPORTED`, `FSIMG_OP_WRITER`, the supplied
   explanation and the entry's rendered error path if any affected field is
   required or `FSIMG_WRITE_ALLOW_LOSS` is clear. Refusal changes no loss
   counter and invokes no loss callback for that mask.
2. Otherwise counts the mask's metadata fields in the writer's cumulative
   planning total, and calls `fsimg_report_loss()` once with that same mask.
   The context primitive owns its cumulative statistics and callback rendering.

A zero mask succeeds without effects. Masks contain known, valid metadata
bits; an absent field cannot be a loss. The internal operation rejects a mask
outside those bounds as an internal state error rather than silently dropping
bits. It does not validate image bytes or replace catalog validation.

The writer has one scalar loss total, initialized to zero before planning.
Common omissions and driver encoding losses both enter this total. Successful
planning copies it into `fsimg_plan_info_t.metadata_losses`; driver plan records
do not independently increment the public count. Saturating addition follows
`fsimg_stat_add()` for both writer and context totals. This removes a separate
omission subtotal and its later addition to a driver-maintained subtotal.

## Units and callbacks

One loss is one valid metadata field on one catalog entry that cannot be
preserved, irrespective of how many components of that field differ:

| Field | One counted unit |
| --- | --- |
| `FSIMG_META_CREATED`, `MODIFIED`, `ACCESSED`, `CHANGED` | One timestamp, including its seconds, fraction and time basis |
| `FSIMG_META_OWNER` | The owner/group pair, even when both identifiers are changed |
| `FSIMG_META_MODE` | The permission field, irrespective of the number of differing bits |
| `FSIMG_META_ATTRIBUTES` | The generic attribute field, irrespective of the number of differing flags |

For example, a QNX4 owner and group both above 65534 produce one owner loss.
A timestamp whose basis, fraction and reproducible value all differ produces
one timestamp loss. Unsupported entry kinds and symbolic-link lengths are
structural failures, never optional metadata losses. Missing values and
format-owned records without catalog metadata produce no losses.

The callback is synchronous and observational: it has no refusal result and
cannot re-enter the context. Its rendered path is borrowed until the callback
returns and is never truncated; errors use the API's bounded rendered path
and truncation flag. Control characters and backslashes receive the common
path rendering, so metadata reporting cannot inject raw terminal controls.
The writer does not print diagnostics or copy paths into an event list.

A mask can report several fields in one callback. Common omissions retain one
callback per entry containing its entire omission mask; individual driver
field conversions retain their individual callbacks. Counts measure set bits,
not callback invocations. The library delivers every accepted loss event when
a callback is installed. CLI sampling or worker transport caps detailed
examples independently and never cap the total.

Context statistics accumulate losses across operations, including observations
made during an operation that later fails. A successful plan's count is local
to that writer. Repeated retrieval of an already completed plan, plan-info
queries and emission do not report or count its losses again.

## Capability omissions and encoding fidelity

The catalog preserves the caller's original metadata. Common planning uses
its unions of entry kinds, valid and required fields, and maximum link target
length to decide whether the selected format's capabilities imply refusal.
These summaries preserve the capability check's existing optimization and
failure behavior:

* When a summary proves refusal, the catalog pass locates the first failing
  entry and attributes the error. It reports no allowed omissions encountered
  on the way. The shared loss operation is invoked only for a refused
  metadata mask in this path; structural failures retain their own checks.
* When no refusal is implied and no field is omitted, there is no extra
  capability catalog pass.
* Otherwise the pass submits each entry's omission mask to the shared loss
  operation before driver planning. A scratch read failure can interrupt this
  pass after earlier callbacks; the summary's no-loss-on-refusal guarantee is
  not a transaction over arbitrary later failures.

For an entry with valid mask `V` and format capability mask `C`, the common
pass owns `V & ~C`. Driver encoders own `V & C`. Drivers do not reinterpret
unsupported fields or submit them as losses. A supported field's encoder
computes its final stored representation and one fidelity result, then submits
its bit once if inexact. Multiple causes of inexactness combine into that one
result. A later layout or emission pass consumes the planned representation
without repeating policy evaluation.

This partition, together with one metadata-encoding pass per catalog entry,
prevents duplicate accounting. There is no per-entry loss ledger, second
normalized catalog, replay queue or generic metadata transformation language.
The format planner retains its existing bounded encoded rows or records, and
geometry sizing does not rerun metadata reporting for candidate layouts.

A capability advertises that the format stores a field, not that it preserves
every possible value. QNX4 owner limits, QNX6 fixed directory permissions,
calendar bounds, subsecond precision and timestamp bases remain driver
representability decisions. Drivers supply format-specific explanations to
the common policy operation rather than duplicating policy branches or path
attribution. Pure codecs return encoding/fidelity results; they do not change
counts or invoke callbacks.

## Defaults and reproducibility

A field absent from `valid` has no caller value to preserve. The driver writes
its documented default without reading the absent value or reporting a loss.
Owner and permission defaults remain independent of process identity and
umask. Timestamp defaults follow the format's explicit policy:

| Format | Non-reproducible fallback | Reproducible selection |
| --- | --- | --- |
| FAT | 1980-01-01 00:00:00, wall-clock basis | Epoch interpreted as wall-clock calendar seconds |
| exFAT | 1980-01-01 00:00:00 UTC | Epoch as UTC |
| QNX4 | UTC host clock sampled once during planning, within 0 through 2147483647 | Epoch within that same interval |
| QNX6 and ImageFS | UTC host clock sampled once during planning, within 0 through 4294967295 | Epoch within that same interval |

The seconds helpers do not sample a clock. Drivers resolve and validate the
fallback once and use it for absent times, out-of-range optional times and
format-owned records as their format design specifies. A host clock outside
the profile range is `FSIMG_ERANGE`, not an optional metadata loss. Invalid
reproducible epochs fail option validation under the format's existing rules.

Reproducible selection and encoding fidelity are separate checks. A valid
original field is exact only when the encoded selected value is exact and
reproducible substitution preserves the original value and its required basis.
Permission to use a reproducible epoch does not waive required-field policy.
An absent field still produces no loss even when its chosen default requires
the format's rounding. `precision_ns` describes the source representation's
resolution; it does not license rounding away nonzero value bits or create a
second loss unit.

## Unsigned whole-second selection

QNX4, QNX6 and ImageFS share a narrow pure helper for selecting an unsigned
32-bit whole-second value. Its inputs are presence, the timestamp when present,
an inclusive maximum, a validated fallback and the reproducible selection
flag. It returns the selected scalar and whether a present value is exact.
The maximum is 2147483647 for QNX4 and 4294967295 for QNX6 and ImageFS; no
format enum or driver callback is needed. The fallback is already the epoch
when reproducibility is enabled.

| Input | Stored seconds | Fidelity |
| --- | --- | --- |
| Absent | Fallback | No requested value; no loss |
| Present, in range, non-reproducible | Original seconds | Exact only for UTC with zero nanoseconds |
| Present, out of range, non-reproducible | Fallback | Inexact |
| Present, reproducible | Fallback/epoch | Exact only for in-range UTC whole seconds equal to the epoch |

The helper checks signed bounds before converting to `uint32_t`. It never
adds a timezone offset: known-offset seconds already describe a UTC instant,
but these profiles preserve only the UTC basis. Thus a known-offset value with
an otherwise identical instant is still inexact, as is a wall-clock value.
An in-range fractional value keeps its integer seconds and drops its fraction;
it does not round to the nearest second. Out-of-range values use the fallback
rather than clamping to an endpoint. The driver writes the returned scalar in
its own byte order and submits an inexact present field to the shared policy.

## DOS calendar conversion

FAT and exFAT reuse `fsimg_dos_time_encode()` from `lib/core/calendar.c` for
the bounded Gregorian conversion. It accepts scalar calendar seconds in
315532800 through 4354819199 inclusive and returns the packed DOS date/time
and odd-second remainder. It is independent of host timezone, locale and
`time_t`; it owns neither field placement, time-basis interpretation,
fractional precision nor loss policy. On an out-of-range input it leaves the
outputs untouched so callers retain initialized fallback bytes.

FAT owns its directory-entry offsets and preserves these fidelity rules:
creation time has 10 ms resolution including the odd second; modification time
has two-second resolution; access time retains only the date. All require
wall-clock basis for exactness. The packed date's upper half and time's lower
half replace the duplicate calendar walk, while the existing field encoders
retain fractional truncation and date-only checks. UTC and known-offset inputs
do not become exact through this reuse.

exFAT retains per-field offset-valid flags, the checked addition of an explicit
offset to obtain calendar seconds, and its 15-minute offset range of -960
through +945 minutes. Creation and modification have 10 ms resolution;
access has two-second resolution. Unrepresentable calendar or offset values
leave the complete initialized UTC fallback, including its fraction and
basis. Ordinary fractional truncation keeps the representable calendar and
offset. Reproducible mode selects UTC and requires the original basis to be
UTC for exactness. These responsibilities remain in the exFAT encoder.

## Failure and lifecycle

Metadata refusal is a planning failure and identifies the responsible catalog
entry. The common loss operation attaches the rendered path itself, so a new
encoder cannot omit attribution. Encoding failures unrelated to optional
metadata retain their status and resource attribution; I/O, scratch,
cancellation, arithmetic and allocation failures never become metadata losses.

Loss callbacks are observations during planning, not promises of a publishable
image. A later required-field refusal, geometry failure, scratch failure,
container planning failure or manifest failure does not retract earlier
callbacks or decrement context statistics. The failing field is not counted.
The writer frees the partial plan, returns no plan, and enters its existing
failed state. There is no inspectable partial plan count and no retry that can
replay losses on that writer. Output publication remains the output
coordinator's responsibility.

## Validation and complexity

Contract tests use independent expected values and cover:

* Allowed, required and forbidden losses for each metadata field; combined
  omission masks; two out-of-range owner identifiers counting once; absent
  fields; fixed permissions; and mixed omission and representability losses.
* Capability-summary refusal before any callback, interruption after earlier
  allowed omissions, and late driver/layout failure retaining only earlier
  context counts. Successful plan counts and repeated plan retrieval agree.
* Callback masks, order, escaped full paths, absent callbacks, context reuse,
  and rejection of context re-entry. Error paths identify the refused entry.
* Whole-second endpoints and adjacent values, fractions, all individual time
  bases, matching and differing reproducible epochs, and validated fallback
  selection. Absent-field tests do not depend on the unused timestamp bytes.
* DOS boundaries, leap days including the non-leap year 2100, odd seconds,
  10 ms truncation, date-only access times, and exFAT offset boundaries and
  checked offset addition. Decoded fields and image bytes match format oracles.
* One encoding/reporting pass across geometry choices, bounded memory on wide
  catalogs, scratch failure, and unchanged reproducible image bytes.

The design adds one writer policy operation and one pure seconds selector,
and reuses the existing DOS scalar converter. It removes per-driver refusal,
counting and attribution branches, duplicate calendar arithmetic, and separate
common/driver count ownership. The remaining driver obligation is concrete:
encode each supported field once and state whether its value is preserved.
No new public option, callback signature, format normalization model or
persistent per-entry state is required.
