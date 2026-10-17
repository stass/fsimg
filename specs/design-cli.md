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
fsimg formats [-j]
```

Input images are seekable regular files. `-` is not an implicit request to
spool stdin into a temporary image, and creation does not promise stdout
support. The library callback API supplies other explicit positional sources.
Source operands for `create` name one host directory; programmatic entries use
the library API.

## Common processing options

| Option | Meaning |
| --- | --- |
| `-S auto|required|off` | Sandbox policy; default `auto`, with a visible warning if unavailable |
| `-T directory` | Scratch parent; CLI default is `TMPDIR` when set, otherwise `/tmp`; `-T -` disables scratch |
| `-j` | Streaming machine-readable records, where supported |
| `-l key=value` | Repeatable resource limit, with each key specified at most once |
| `-m bytes` | Aggregate fsimg memory budget; default 256 MiB |
| `-v` | Progress/capability diagnostics on stderr |

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
| `-O key=value` | Typed format settings; shared keys include `sector` and `cluster`, specified in bytes |
| `-R` | Reproducible image bytes |
| `-e epoch` | Timestamp epoch for `-R`; default Unix second 315532800 (1980-01-01 UTC) |
| `-f bytes` | Minimum free-space reserve after metadata and allocation rounding |
| `-n` | Collect, validate, and plan; report the plan without creating an output image |
| `-o image` | Required output pathname except with `-n` |
| `-s bytes` | Positive exact logical volume size; omission selects automatic sizing |
| `-t format` | Required writer format |

`-e` requires `-R`. Reproducible mode normalizes timestamps to the selected epoch,
uses defined stable identifiers, and fixes ordering/padding. It does not read
the host timezone, locale, or `SOURCE_DATE_EPOCH` implicitly. Without `-R`, the
declared source timestamp policy and a generated volume identifier apply.
Geometry candidates and format defaults are reported in the plan.

Host import includes regular files and directories, content, hierarchy, and
representable timestamp fields. It rejects symlinks and special files. It does
not advertise POSIX ownership/mode fidelity on FAT/exFAT. Hardlinked regular
host files are copied as independent files; source inode identity is not an
image hardlink capability.

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
still exposes image metadata. No force flag, lossy name rewriting, or sandbox
opt-out weakens the path and file-type checks.

Logical output bytes, including holes, count against the output quota. Sparse
files are used where possible. Success means all selected content and required
finalization completed. On error, a private partial destination can remain;
the diagnostic identifies it. There is no all-or-nothing directory publication
or resume promise.

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
