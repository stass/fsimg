# ISO 9660 specification harness coverage

The ISO plug-in manually translates requirements in
[the format design](../specs/design-iso9660.md) into Ruby rule values and
predicates. It does not parse specification prose. Every rule names its
specification anchor, rejection class and checking scope. Its domain supplies
an independent valid witness and a mutation that violates the rule. The generic
harness derives expectations; ISO cases carry no hand-written expected status.

There are 65 declared rules, including eight source-input rules. The
`test_every_rule_has_a_sound_positive_and_negative_witness` test classifies
both sides of every domain and checks that the intended violation is found.
Cases use the shared registry, runner, stage comparison, failure recording and
replay. Exact `iso9660` lookup precedes the numeric-family lookup for FAT.

## Rule map

Identifiers below have the `iso9660.` prefix. Every rule's `source` field
names its precise format-design heading.

| Requirements | Rules | Checking scope |
| --- | --- | --- |
| Descriptor identity, supported profile, scan bound, terminator and primary agreement | `identity`, `descriptor.identity`, `descriptor.profile`, `descriptor.limit`, `descriptor.terminator`, `primary.present`, `primary.duplicates` | Open |
| Numeric copies, geometry, embedded root and dates | `numeric.copies`, `geometry.profile`, `geometry.volume`, `root.descriptor`, `root.range`, `descriptor.dates` | Open |
| Joliet descriptor agreement/count and decoded identifiers | `joliet.geometry`, `joliet.count`, `names.joliet` | Open; directory or verification |
| Record framing, copies, profile, flags, dates and dot relations | `directory.framing`, `directory.copies`, `directory.profile`, `directory.flags`, `directory.dates`, `directory.dots` | Embedded root at open; directory or verification |
| Primary identifiers, displayed-name/extent uniqueness and physical order | `names.primary`, `names.unique`, `primary.order` | Directory or verification |
| Sections and complete allocated payload bounds | `sections.framing`, `sections.ascending`, `payload.bounds` | Directory; verification for an unselected hierarchy |
| L/M tables, optional copies, parent/tree correspondence and metadata/data ownership | `path.tables`, `ownership` | Verification |
| SUSP framing, root ER shape, raw CE numeric copies/offsets, overlap and continuation/skip profile | `susp.framing`, `susp.er`, `susp.ce`, `susp.ce_profile`, `susp.skip` | Handshake at open; directory or verification |
| RR entry version/length, PX copies, TF dates, singletons, kind and PN applicability | `rr.version`, `rr.lengths`, `rr.px`, `rr.tf`, `rr.singletons`, `rr.kind`, `rr.kind_profile`, `rr.unsupported` | ER handshake at open; interpreted records at directory preparation |
| NM continuation, names and reader name bound | `rr.names`, `rr.name_profile` | Directory |
| SL framing, continuation, component grammar and reader profile | `rr.sl.framing`, `rr.sl.components`, `rr.sl.profile` | Directory |
| Safe link/FIFO extraction refusal and omitted-name refusal without `-k` | `extract.safe_kinds`, `extract.omitted_names` | Destination; the witness also lists successfully |
| CL/PL numeric copies, parent relations, duplicate and orphan relocation targets | `rr.pointers`, `relocation.parents`, `relocation.targets` | Directory; verification |
| El Torito checksum/signature, supported platforms/media, authoritative section counts and catalog bound | `boot.validation`, `boot.profile`, `boot.sections`, `boot.entry` | Open |
| Writer alignment/settings, names/targets, boot paths/load, level-dependent sections, provable capacity lower bound and ceiling | `create.geometry`, `create.name_bytes`, `create.target_bytes`, `create.boot_paths`, `create.bootload`, `create.file_sections`, `create.volume_lower_bound`, `create.volume_ceiling` | Input |

The builder owns a small fixed address map; the decoder follows on-disc
pointers independently. It shares no generator code with the native C tests.
Fixtures include primary, Joliet and RR trees, subdirectories, continuations,
relocation, multi-extent files, links, FIFOs and BIOS/EFI catalog sections.
Deterministic link boundaries include 1,023- and 1,024-byte decoded targets,
root components, repeated separators, dot and parent components. Seeded
PropCheck draws combine existing builder features. Every draw is classified;
the configured sample and every generator defect run as ordinary pending cases.
Their recipes retain the seed and feature parameters.

Accepted creations run every reading stage, compare kind/size/content and
verify counters, and reproduce byte for byte from reversed manifest lines.
Link size and hash derive from immutable target bytes; FIFO content fields
are forbidden. Special kinds require a manifest root, so fixture preparation
never creates host links or FIFOs. Settings are separate literal argv values,
including paths with commas or equals signs. FAT rules refuse those settings
and kinds before its layout arithmetic.

## Independent tools

`FSIMG_TOOL_ISOINFO` selects `isoinfo -d` for declared geometry;
`FSIMG_TOOL_XORRISO` selects `xorriso -no_rc -indev ... -pvd_info` for an
independent load. The oracle shell bounds process lifetime and output. Missing
tools produce unavailable observations. Reading-base observations are recorded;
a disagreement on a created image fails its case. These adapters do not claim
file-content read-back. The writer qualification suite owns detailed consumer
listings, metadata, content digests, boot and kernel-reader qualification.

cdrtools `isoinfo` 3.02a09 reports `Short read on old image` on valid tiny
images: its descriptor window requires 48 physical blocks. The same 27-block
image fails at physical lengths through 47 blocks and succeeds with zero
trailing bytes at 48 blocks, without changing its declared volume. The adapter
records `not_applicable` only below that threshold, with exit 255, empty stdout
and that exact stderr diagnostic. It never pads an image, changes the writer
minimum or treats another failure as this exception. xorriso observes the
original image.

## Oracle boundaries

Metadata reads and derived paths share a cumulative work budget; payload
digests have a separate byte ceiling. Exhaustion leaves dependent scopes
explicitly unclassified. Complete structures retain invalid raw facts for rules
to reject. A traversal failure does not prevent a decoded local diagnosis.

Exact explicit-capacity acceptance is unclassified when locally modeled input
constraints pass. The source model proves refusals for alignment, format
ceilings and a lower bound consisting of fixed metadata plus rounded regular
payload. It does not duplicate the full writer planner. Valid creation witnesses
use automatic sizing. Native writer tests and their independent checker own
exact-fit/one-short boundaries, padding, unused bytes, large files and deep
relocation layout.

The plug-in compares the automatically selected view. The shared recipe has no
ISO `view` or label setting; native driver and integration tests cover those.
Generic tree comparison checks kind, size and content, rather than every
uid/gid/mode/timestamp field. RR field validity is independently classified;
the writer qualification suite checks detailed reported metadata. The bounded
feature generator is not an arbitrary ISO directory/name generator. It does not
claim exhaustive Unicode omission/normalization trials, every deferred-refusal
precedence combination or the 65,536-section refusal boundary; those are
separate native and integration test responsibilities.

## Checks

Focused checks use the ordinary harness entry point:

```sh
FSIMG_BUILDDIR=/path/to/build FSIMG_BIN=/path/to/build/fsimg \
BUNDLE_GEMFILE=/path/to/source/test/spec/Gemfile \
ruby test/spec/run.rb --profile=quick \
  test/spec/suite/iso9660/test_*.rb \
  test/spec/suite/core/test_special_sources.rb \
  test/spec/suite/core/test_recipe.rb \
  test/spec/suite/core/test_tree.rb \
  test/spec/suite/core/test_manifest.rb
```

The configured `test/spec.sh quick` wrapper supplies the full build/tool
environment. `make -j32 check` runs its quick profile. Sanitizer, fuzz, complete
CI and platform gates run on the integrated tree, following [build.md](build.md).
