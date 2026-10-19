# fsimg command-line contract

## Command structure

The command is `fsimg`. Options use `getopt(3)` and short flags. Common flags
retain their meaning across subcommands. Invalid options, repeated singleton
options, unknown format settings, and unexpected operands fail before image
processing. `--` terminates option parsing. No command modifies an existing
image in place.

```text
fsimg create [options] -t format -o image source_directory
fsimg info [options] -i image
fsimg ls [options] -i image [image_path]
fsimg cat [options] -i image image_path
fsimg extract [options] -i image -C new_directory [image_path]
fsimg verify [options] -i image
fsimg formats [-jv]
```

Without arguments, fsimg prints the command overview to stderr and exits 2.
`-h` displays the overview; a command's `-h` displays only that command's
synopsis, options, and examples on stdout. Syntax errors name the problem and
include the applicable synopsis on stderr. Top-level `-V` displays the version. Options precede
operands on every host; `--` permits an operand beginning with a dash.

Input images are seekable regular files. `-` is not an implicit request to
spool stdin into a temporary image, and creation does not promise stdout
support. The library callback API supplies other explicit positional sources.
Source operands for `create` name one host directory; programmatic entries use
the library API.

## Common processing options

These options apply to processing commands. `formats` accepts `-j`, `-v` and
`-h`, as shown in its synopsis.

| Option | Meaning |
| --- | --- |
| `-S auto|required|off` | Sandbox policy; default `auto`, with a visible warning if unavailable |
| `-T directory` | Scratch parent; CLI default is `TMPDIR` when set, otherwise `/tmp`; `-T -` disables scratch |
| `-j` | Streaming machine-readable records, where supported |
| `-l key=value` | Repeatable resource limit, with each key specified at most once |
| `-m bytes` | Aggregate fsimg memory budget; default 256 MiB |
| `-v` | Bounded progress, capability details and sampled metadata-loss examples on stderr |

`-l` keys are `scratch`, `entries`, `depth`, `path`, `fds`, `output`, `work`,
and `seconds`. Scratch/path/output values are byte quantities; entries/depth/fds/
work are unsigned counts; seconds is an integer wall-clock deadline, with zero
selecting the default of no additional deadline.
Work counts primitive metadata steps and adapter calls, not just payload bytes.
The deadline is also supervised by the coordinator. Unset work/time limits
retain algorithmic traversal bounds and cancellation, not a hidden short
timeout that makes gigantic legitimate jobs impossible.

Zero resource values select their documented defaults; disabling scratch uses
`-T -`. There is no magic negative or `unlimited` operand. Explicit large limits
remain bounded by checked `uint64_t` counters and host capabilities. The
[architecture](architecture.md#bounded-resources) defines default limits.

Byte quantities are unsigned decimal with an optional `K`, `M`, `G`, `T`, or
`P` binary multiplier (powers of 1024). Bare values are bytes. Signs, whitespace
inside a number, fractions, percentages, overflow, and trailing junk fail.
Counts and Unix epochs have their separately documented integer syntax.

For massive catalogs, `-T` should identify disk-backed storage. A tmpfs/RAM disk
can consume system memory outside fsimg's heap budget. Diagnostics report
memory and scratch budgets independently and identify unavailable scratch or
host capacity errors. Scratch is for metadata, not payload staging.

## Read-side options and selection

`info`, `ls`, `cat`, `extract`, and `verify` accept:

| Option | Meaning |
| --- | --- |
| `-b bytes` | Volume start offset within the image; default zero |
| `-i image` | Required image pathname |
| `-t format` | Explicit decoder; default is unambiguous detection |
| `-z bytes` | Volume slice length; default is the remaining image length |
| `-O key=value` | Repeatable typed format option, such as `oem=cp437` for FAT |

Offset and length form one bounded slice and are validated against actual file
length. Raw volumes are the default. fsimg does not auto-select a partition or
silently guess among multiple filesystem candidates. Format forcing retains
all safety checks. Unknown format-option keys and unsupported values fail.
An explicitly supplied slice length is positive. The supported read setting
is `oem=cp437`; creation settings are `sector` and `cluster` byte quantities.

An image path is an absolute validated UTF-8 path; omitted selection means `/`.
Matching follows each format's component lookup rules, including FAT case
equivalence and short aliases. Results retain displayed image paths. `ls` lists immediate children of a selected
directory, or the selected file; `-r` recursively lists its subtree. The reader
may scan preceding metadata to find a path, but does not read unselected payloads
or build a whole-volume pathname hash table. A missing selection is an error.

`cat` requires one regular file and writes its logical bytes to stdout,
including explicit zero bytes for logical zero ranges. Its content is raw;
terminal escaping would corrupt it. It rejects `-j`. Broken output pipes and
content errors produce failure rather than a truncated success result.

## Creation

Creation accepts common options and:

| Option | Meaning |
| --- | --- |
| `-L label` | Explicit volume label; default `FSIMG`, subject to format rules |
| `-M` | Minimize the image: choose the open geometry settings that give the smallest volume; conflicts with `-s` |
| `-O key=value` | Typed format settings; shared keys include `sector` and `cluster`, specified in bytes |
| `-R` | Reproducible image bytes |
| `-e epoch` | Timestamp epoch for `-R`; default Unix second 315532800 (1980-01-01 UTC) |
| `-f bytes` | Minimum free-space reserve after metadata and allocation rounding |
| `-n` | Collect, validate, and plan; report the plan without creating an output image |
| `-o image` | Required output pathname except with `-n` |
| `-p` | Import host owner, group and permission bits as optional metadata |
| `-s bytes` | Positive exact logical volume size; omission selects automatic sizing |
| `-t format` | Required writer format |

`-e` requires `-R`. Reproducible mode normalizes timestamps to the selected epoch,
uses defined stable identifiers, and fixes ordering/padding. It does not read
the host timezone, locale, or `SOURCE_DATE_EPOCH` implicitly. Without `-R`, the
declared source timestamp policy and a generated volume identifier apply.
Geometry candidates and format defaults are reported in the plan.

Host import includes regular files, directories and symbolic links, content,
hierarchy, and representable timestamp fields; `-p` adds owner, group and
permission bits. It rejects special files. A format that cannot store a kind
fails planning; a field it cannot store is an optional loss, counted in the
usual notice, so FAT and exFAT advertise no POSIX fidelity and `-p` never
changes their image bytes. The default label applies only to formats
with a label. Hardlinked regular host files are copied as independent files;
source inode identity is not an image hardlink capability.

Scanning rejects descendant mount crossings. The creation sandbox has broader
source-root authority than the image-reading sandbox; read-only access alone
does not prevent opening device nodes already beneath an authorized root.
For adversarial sources requiring device-open exclusion, use an isolated
filesystem with device nodes disabled. This
[profile limitation](design-security.md#source-root-profile) is reported with
creation capabilities; ordinary scanning uses nonblocking/no-follow opens and
checks type before reading.

The output must not already exist. Planning completes before image-file
creation; payloads are written once to a staged image on the destination
filesystem. Final publication is atomic and without replacement where the
supported publication primitive can establish that guarantee. There is no
force-overwrite flag. A failed operation reports whether a staged or published
output exists and whether durability is uncertain.

`-n` may use metadata scratch but does not create the output image or read file
payloads. It reports logical size, allocation geometry, payload/metadata/free
bytes, entry count, and known resource estimates with estimates labeled. It
cannot certify future source stability or the availability of future disk space.

## Extraction

`-C` names a new destination directory; even an existing empty directory fails.
The selected entry/subtree retains its path below the image root, including
necessary parent directories. The default extracts the entire volume.

The CLI uses the [safe content profile](design-security.md#safe-extraction):
private directories and files, regular content only, no links, no privileged
metadata, no existing-tree merge, and no host timestamp restoration. Inspection
still exposes image metadata. An entry of another kind, such as a symbolic
link, fails the extraction unless `-k` is given, which skips such entries and
reports their count. No force flag, lossy name rewriting, or sandbox
opt-out weakens the path and file-type checks.

Logical output bytes, including holes, count against the output quota. Sparse
files are used where possible. Success means all selected content and required
finalization completed. On error, a private partial destination can remain;
the diagnostic identifies it. There is no all-or-nothing directory publication
or resume promise.
Final report delivery is part of command completion. If that delivery alone
fails, extracted content may already be complete, while the command still
conservatively reports incomplete output.

## Inspection, verification, and capabilities

`info` reports detected format/profile, volume and allocation geometry, label,
implemented capabilities, and validation scope. It does not claim to verify
the full filesystem or scan free data space. Counts requiring traversal are
marked unknown rather than triggering an undisclosed full scan.

`verify` checks the supported profile's complete structure. `-d` additionally
reads initialized payload bytes; logical zero tails need no backing read.
Verification reports unsupported critical features, corruption, incomplete
work, and resource limits distinctly and never repairs an image.

`formats` reports compiled-in reader/writer capabilities, format profiles,
geometry restrictions, metadata fidelity, and available sandbox backends.
Availability distinguishes a compiled backend from a usable runtime facility.
Formatting a volume and booting a machine from it are separate capabilities;
fsimg does not promise to install boot loaders.

## Output, diagnostics, and exit status

Human output is concise and terminal-safe; names and control characters are
escaped unambiguously. Diagnostics and progress go to stderr. Payload output
and data records go to stdout. Progress has bounded frequency and can be disabled;
it never accumulates one record per processed file in memory.

Successful creation and extraction are silent by default. Human listings have
one quoted, escaped pathname per line and no completion trailer. `info` uses
aligned labels and binary byte units, identifies unknown counts without an
implicit scan, and states header-only validation. `formats` shows a compact
read/write/verify table; `-v` adds geometry, metadata and sandbox capability
details on stderr. Creation plans appear with `-n` on stdout or with verbose
creation on stderr. Verification emits one human result only after successful
worker exit and finalization, stating structural or initialized-payload scope.
Verbose completion details use stderr. Human success is determined by exit
status; the mandatory machine completion record belongs to `-j`.

Human errors use `fsimg: explanation`, optionally with a quoted pathname and
an OS error description. Prose has no enclosing quotes. Empty internal fields,
numeric errno values and generic status prefixes do not appear. Resource
failures identify the resource, limit and observed demand. Actual cleanup
failures, retained output/scratch paths, incomplete output, published output
and uncertain durability remain visible in plain language. Truncated paths
and messages are labeled; terminal escaping applies to prose as well as names.
FAT layout failures identify the rejected constraint when all candidates fail
for the same reason; otherwise they describe the conflicting requested settings
without guessing a remedy or an uncomputed minimum image size.

Optional metadata loss on success produces one final notice counting affected fields,
not files. Verbose examples name fields and are explicitly sampled; truncated
path excerpts are labeled. Samples never establish exact totals. Failed commands
report the primary error and required output/cleanup consequences without an
additional metadata-loss notice. Verbose or JSON samples can precede failure.
FAT sizing errors explain the limiting constraint; automatic-size sentinels and
unrequested free-space reserves do not appear as zero-byte quantities.
Sandbox-unavailable/disabled warnings remain visible before
processing regardless of verbosity or JSON mode. Source profile limitations
remain documented and appear in verbose creation/capability details.

`-j` writes newline-delimited JSON records with a schema version and `type`.
Entry records stream individually, followed by a completion summary. There is
no JSON array proportional to entry count. Final records include validation
scope, counters, selected limits, metadata-loss counts, and sandbox status.
An error emits a bounded error record when the stream is usable; absence of
a successful completion summary means incomplete output. JSON numeric values
that can exceed exact binary64 integer range are decimal strings with documented
units. Human-readable sandbox warnings remain visible on stderr.

Exit status is 0 for complete success, 1 for operational failure, and 2 for
invalid command syntax. Corruption, unsupported required features, partial output,
and uncertain durability are failures. A sandbox-unavailable warning under
`auto` alone does not make otherwise successful work fail. Signals/cancellation
stop the worker, suppress publication, and report incomplete output when possible.

## Examples

```sh
fsimg create -R -t fat32 -s 1G -L BOOT -o boot.img ./rootfs
fsimg create -R -t exfat -f 100G -m 128M -T /scratch -o data.img ./files
fsimg create -n -R -t exfat -m 64M -T /scratch ./files
fsimg info -i data.img
fsimg ls -j -r -i data.img /
fsimg cat -i boot.img /EFI/BOOT/BOOTX64.EFI > BOOTX64.EFI
fsimg extract -S required -i data.img -C ./new-output /documents
fsimg verify -d -m 128M -T /scratch -i data.img
fsimg info -b 1048576 -z 1073741824 -i disk.img
fsimg formats -j
```

## Coordinator and streaming representation

The coordinator parses trusted command operands, prepares private capabilities,
and supervises its same-executable worker before publishing an image. It never
parses image bytes. The existing output relay owns every potentially blocking
stdout/stderr write, including help, capability records, errors and final
summaries. An output-only producer reuses the same relay queue, poll loop,
confirmed-byte accounting, signal handling and absolute monotonic deadline.

Command parsing owns singleton and key-duplicate detection. Numeric parsing
uses checked decimal accumulation and explicit binary byte suffixes. Only
known commands/options/settings reach descriptor preparation. Read operations
share the existing worker selector. Create dry runs supply a source root and
optional scratch, never output authority. Normal creation prepares a fresh
private staging directory on the final filesystem; its worker creates the
image only after planning. Only the coordinator publishes, after a validated
completion and successful worker exit. Extraction prepares a fresh private
root and retains it, with an explicit incomplete outcome on failure.

Scratch is a separate fresh private tree beneath -T/TMPDIR or /tmp. It is never
an output ancestor or descendant. Cleanup never recursively follows worker or
image names. Worker output authority cannot remove output. Scratch cleanup
uses descriptor-relative no-follow operations on coordinator-created trees
after children exit. Cleanup failures preserve the primary error and identify
retained private output/scratch locations.

Output formatting retains one 128 KiB span, one small UTF-8 carry, the current
entry metadata, and fixed result/counter records. Name frames stream within
JSON strings; no pathname or listing tree proportional to entries is retained.
Valid UTF-8 crosses name-frame boundaries intact. Human names escape controls,
backslashes and bidi formatting unambiguously. JSON uses valid UTF-8, escapes
JSON controls/quotes, and encodes untrusted invalid diagnostic bytes visibly.
Under `-j`, only the successful final summary certifies completion. An error after a
partial entry first closes that incomplete record, then emits an error record;
no successful summary follows. Counts and byte quantities use decimal strings
in schema version 1, and all fields state their units. Header, selected-entry,
structural and payload-read scopes remain separate.

Capability records distinguish the static format profile and compiled sandbox
backend from actual per-operation readiness; an unprobed runtime is unknown.
Source-root limitations and timestamp/source consistency contracts are explicit.


The command implementation separates parsing (options.c), bounded text
formatting (display.c), capability preparation/publication (main.c and paths.c),
and existing worker/protocol/sandbox owners. The command links the same core
objects statically so fresh worker execution needs no loader environment.
The public shared/static library remains independently installed and tested.

Schema version 1 records use numeric schema=1 and string type. Byte quantities,
counts, Unix seconds and nanosecond precision are decimal strings; bounded
kind/flag fields, nanoseconds, offset minutes, geometry sizes, enumerated size
arrays and the UTF-16 name-unit limit are JSON integers. Unknown
values are null. A timestamp explicitly includes basis and resolution. Record
types are info, plan, entry, verify, format, capabilities, loss, progress,
error and summary. Progress and sampled loss details use stderr; data and
summary/error records use stdout under -j. Raw cat rejects -j and directs any
final diagnostic summary to stderr. Entry records carry complete=true only
after all name frames and ENTRY_END validate; error closure marks them false.
No final successful summary is emitted before worker exit and publication.

CLI help uses -h, and top-level version uses -V. Typed writer settings are
sector and cluster in bytes; FAT oem=cp437 is explicit on read commands.
Explicit slice length and exact image size are positive. Offset can be zero.
Resource zero selects its documented default. Singleton and key duplicate
flags are syntax errors even if values agree. -e requires -R. -r only applies
to ls; -d only applies to verify; -j is rejected by cat. Parsing requires all
options before operands (or an explicit --), consistently on all target hosts.

The signed Unix epoch syntax permits an optional leading minus followed by
decimal digits. It rejects a plus sign and byte suffixes. Representability in
the chosen filesystem remains a planning check.

The operation deadline includes successful output. Once processing fails,
diagnostic delivery has a separate two-second grace; one further two-second
stderr-only attempt applies if stdout cannot carry the report. Neither attempt
can resume processing or publish output. Scratch cleanup removes at most 4,096
flat entries and never descends into a worker-created directory. A retained
scratch directory or staging object is reported explicitly.

`-l output` limits logical file bytes, including holes, rather than escaped
listing text. Formatted stdout and stderr share the separate derived ceiling
in the architecture. Summaries report its ceiling, queued text and confirmed
stream bytes. Every reported ledger counter is a snapshot before its own record
is queued, so it excludes that reporting record. Error reporting can use the
reserved final 128 KiB. Progress emits only
when an entries/bytes/work counter reaches a new power-of-two milestone, in
addition to its frequency limit; it therefore needs at most 193 records per
operation even for gigantic payloads. Suppressing redundant progress never
stops processing. The common relay charges each text span before forwarding it,
and preserves the aggregate ledger across worker and final-reporting phases.
