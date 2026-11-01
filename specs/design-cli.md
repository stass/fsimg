# fsimg command-line contract

## Command structure

The command is `fsimg`, shaped like `tar(1)`: one mode letter chooses the
operation, `-f` names the image, and the remaining words are operands. The
goal is tar-like ergonomics, not tar parity: letters tar users type by habit
keep their tar meaning, fsimg's own settings keep their letters where they do
not collide, and rare settings are long options.

```text
fsimg c[W] -f image -F stack [options] [-C dir] operand ...
fsimg x -f image [options] [-C dir] [member ...]
fsimg x -O -f image [options] member
fsimg t -f image [options] [member ...]
fsimg W -f image [options]
fsimg i -f image [options]
fsimg --formats [--json] [-v]
fsimg -h | -V
```

| Mode | Operation |
| --- | --- |
| `c` | Create an image from host operands |
| `x` | Extract members into a fresh destination, or one member to stdout with `-O` |
| `t` | List members, recursively by default |
| `W` | Verify an image; as `cW`, verify the staged result before publication |
| `i`, `--info` | Report the stack, partition table and volume information |
| `--formats` | Report compiled and usable capabilities; takes no image |

Exactly one mode is given, except that `c` and `W` combine. No mode, two
modes, an option a mode does not accept, a repeated singleton option, a
repeated setting or limit key, an unknown stack or setting name and an
unexpected operand are syntax errors: they fail before any image or source
is opened, name the problem, print the applicable synopsis on stderr and exit
2. Without arguments fsimg prints the overview on stderr and exits 2. `-h`
with a mode prints that mode's synopsis, options and examples on stdout, `-h`
alone the overview; `-V` prints the version. No mode modifies an existing
image in place.

### Bundled mode letters

`argv[1]`, and only `argv[1]`, may be a tar-style bundle: letters without a
leading dash, such as `xvf`. The parser decides in this order:

1. A first argument beginning with `-` is an ordinary option word;
   `fsimg -xvf boot.img` works through ordinary `getopt` bundling.
2. Otherwise every character must be a short option letter. Any other
   character is a syntax error naming it, so a mistyped word such as
   `extract` fails with the overview instead of being half-interpreted.
3. The bundle must contain a mode letter or `h`; none, or two modes other
   than `cW`, is a syntax error.
4. The bundle is rewritten into one `-X` word per letter, in order. A letter
   that takes an argument consumes the next unconsumed word after the bundle,
   in letter order: `fsimg cfF boot.img fat32` is
   `fsimg -c -f boot.img -F fat32`. A missing argument is a syntax error.

The rules below then apply unchanged, so a bundle expresses nothing that
dashed options cannot.

### Long options and ordering

Options are parsed with `getopt_long(3)`. Short letters serve the tar-like
and frequent options; rare and new settings are long-only. Short options
have no long aliases, except that `--info` names mode `i` for symmetry with
`--formats`. `--` ends option parsing.

Options precede operands on every host: the option string begins with `+`,
which stops parsing at the first word that is not an option, so a later `-x`
is an operand. The `+` is needed everywhere, because glibc, musl, FreeBSD and
macOS `getopt_long` all permute arguments by default; `POSIXLY_CORRECT` is
not consulted. One exception is deliberate: in mode `c` the operand list may
contain further `-C dir` words (also spelled `-Cdir`), applying to the
operands after them.
After `--` every word is an operand, including one spelled `-C`.

## Common options

| Option | Meaning |
| --- | --- |
| `-f image` | The image: input for `x`, `t`, `W`, `i`; output for `c`; `-` is stdin or stdout. Required except with `c -n` |
| `-C dir` | An existing directory: the parent of the extraction destination; the base of create operands |
| `-S auto|required|off` | Sandbox policy; default `auto`, with a visible warning when confinement is unavailable |
| `-T directory|-` | Scratch parent; default `TMPDIR` when set, otherwise `/tmp`. `-T -` disables scratch: no spool is created, so gzip input then fails with a diagnostic naming `-T -` rather than a limit figure; with `-f -`, whose stream needs the scratch tree for its spool or raw intermediate, and with `cW` of a compressed image, whose verification reads it through the spool, `-T -` is a syntax error |
| `-l key=value` | Repeatable resource limit; each key at most once |
| `-m bytes` | Aggregate fsimg memory budget; default 256 MiB |
| `-v` | Repeatable verbosity, defined per mode below |
| `--json` | Streaming machine-readable records on stdout, where supported |
| `-h`, `-V` | Help and version |

`-l` keys are `scratch`, `spool`, `entries`, `depth`, `path`, `fds`,
`output`, `work` and `seconds`. Scratch, spool, path and output values are
byte quantities; entries, depth, fds and work are unsigned counts; seconds is
an integer deadline measured on the monotonic clock, where zero selects the
default of no additional deadline. `spool` bounds the temporary file that
holds a stream or compressed input, defined in the
[layer design](design-layers.md#budget). No limit of its own bounds the
manifest `x --mtree` writes or a `t` listing: `entries` bounds their lines
and `path`, with the 1024-byte link-target limit, each line's length
([manifest design](design-mtree.md#security-and-bounds)).

`fds` defaults to the depth limit plus 128. A source scan holds one
descriptor per directory level along its path, and so does an extraction, so
the default fits a tree as deep as the depth limit, whatever depth the
command gives; the 128 cover everything else that a command with up to
about a hundred operands holds at once. A command with more operands is
admitted while its open roots and fixed descriptors fit, and scanning a path
at the depth limit can then still reach the limit and fail, naming the
descriptor limit. The
library keeps its own fixed default for embedders, whose descriptors it
shares with the host program. `fds` also caps the number of create operands,
since the worker holds one open root per operand, two for a manifest operand
with a content directory (the manifest and the directory), besides its own
scratch directory, the staging directory of an image written to a file, the
[raw intermediate](design-layers.md#handed-over-files) of a container,
compressed image or stream, and two scanning descriptors (a directory and
the entry read from it); a command line leaving no room for those fails
before the worker starts, naming the descriptor limit. The coordinator opens
the operands within the same limit before it starts the worker. The host's
`RLIMIT_NOFILE` still applies to both processes, and fsimg never raises it.

The worker charges every descriptor it holds, each root and handed-over file
included, from the moment it holds it, so that limit counts them all. An
internal adopter takes the worker's charge over with the descriptor; a
public library call that adopts a handed-over file charges it itself, so the
worker returns its charge as it passes the file over. Work counts primitive
metadata steps, adapter calls and every byte decompressed from a container
chunk or a gzip member, not just payload bytes, so `work` and `seconds`
bound decompression time. The deadline is also supervised by the
coordinator. Unset work and time limits retain algorithmic traversal bounds
and cancellation, not a hidden short timeout that makes gigantic legitimate
jobs impossible.

Zero resource values select their documented defaults; disabling scratch
uses `-T -`. There is no magic negative or `unlimited` operand. Explicit
large limits remain bounded by checked `uint64_t` counters and host
capabilities. The [architecture](architecture.md#bounded-resources) defines
the defaults, except the CLI's `fds` default above.

Byte quantities are unsigned decimal with an optional `K`, `M`, `G`, `T` or
`P` binary multiplier (powers of 1024). Bare values are bytes. Signs,
whitespace inside a number, fractions, percentages, overflow and trailing
junk fail. Counts are unsigned decimal without suffix. The signed Unix epoch
permits an optional leading minus and decimal digits, rejecting a plus sign
and suffixes; representability in the chosen filesystem is a planning check.
Singleton and key duplicates are syntax errors even when the values agree.

Scratch, and any payload-sized temporary file placed there, consumes
system memory outside fsimg's memory budget when `-T` names a tmpfs or RAM
disk, so `-T` should name disk-backed storage. Diagnostics report memory, scratch and spool
budgets independently and identify unavailable scratch or host capacity
errors. What scratch and the two payload-sized temporary files hold, and
where each lives, is the [storage design's](design-storage.md#resource-model)
rule.

### Verbosity

| Mode | `-v` | `-vv` |
| --- | --- | --- |
| `c`, `x` | One escaped entry name per line on stderr as each entry is processed | Adds, on stderr, the creation's plan with its chosen geometry (the cluster or block size and count and, for QNX6, the inode capacity), bounded progress, capability details and sampled metadata-loss examples |
| `t` | The long listing on stdout, defined under [listing](#listing) | The same stderr details |
| `W`, `i`, `--formats` | Capability, geometry and sandbox details on stderr | Nothing further |

A `c -v` or `x -v` name line is the default `t` line on stderr: the quoted,
escaped image path, a symbolic link marked `@` and a FIFO `|`, never a host
path. The names are the library's
[entry callback](design-api.md#options-context-and-callbacks), relayed
through the listing's entry records, so one escaping, one path limit and one
entries limit serve every mode. Creation names each entry as planning admits
it to the catalog, in image order: the lines are the `t` listing of the
image to come, less the entries a format adds itself rather than takes from
the catalog, such as QNX6's synthesized `/.boot` or QNX4's `/.boot` and
`/.altboot`, and `c -n -v` names them too, before the plan. Extraction
names each entry once it is created or copied, and recorded in the manifest
when there is one, in traversal order; under `-k` a skipped entry is named
as well, and since the safe profile creates only regular files and
directories, a marked line under `x` is a skipped entry. An entry the
reader omits has no name to print and appears only in its count. An entry
whose extraction fails is not named; the error names it. Creation names an
entry when the catalog admits it, before the plan's checks, so a creation
can fail naming an entry already listed. Under `--json` stdout's records are
unchanged and the names take the `entry` record form on stderr, as progress
and loss examples do; a creation's record carries the catalog's metadata,
before the format reduces it.

Verbose names are streamed, never accumulated: the worker holds the
library's borrowed entry, the coordinator one formatted span and the relay
one record, so a terminal slower than the job slows the job rather than
losing names, exactly as it slows `t`, and the deadline still applies. A
mode that dropped names under pressure would need a drop count, a frame
and a notice for a condition the user created by asking for names.

## Stack, settings and selection

`-F stack` names the layers of an image inner to outer, separated by `/`,
like file suffixes read left to right: `fat32`, `fat32/gpt`, `fat32/gpt/dmg`,
`exfat/mbr/vhd`, `fat32/dmg`. The [layer design](design-layers.md#the-stack)
owns the grammar: three slots in fixed order, filesystem, scheme, container;
a name fills the slot of its kind and `-` marks the next slot empty, so
`fat32/-/dmg` spells `fat32/dmg` out and `fat32/-/-` is a bare volume. A
repeated kind or misordered name is a syntax error, not a guess. Filesystem
names are `fat12`, `fat16`, `fat32`, `exfat`, `qnx6`, `qnx4`, `ifs`,
`hfsplus`, `hfsx`, `apfs`, `udf` and `iso9660`.

`-F` is required for `c`, and its stack must begin with a filesystem name:
`-F gpt` creates nothing and is a syntax error. On reading, a named slot is
forced, a `-` forces its slot empty, and an unfilled slot is detected, so
`-F fat32` forces the FAT decoder on whatever volume the container and
partition table around it yield, while `-F fat32/-/-` forces every slot and
leaves only compression to the layer design's step-1 rule. Without `-F`,
every slot is detected unambiguously or the command fails. Forcing retains
every safety check.

Compression is not a slot: it wraps the whole image on its way to or from
the file. On create, `--compress=NAME` selects the compression the registry
registers as NAME (`gzip`, `zstd`, `xz`), `-z` is its shorthand for
`--compress=gzip`, and `-a` chooses the compression from
the output suffix as tar does, using the suffixes the layer registry
declares: `.gz` selects gzip, `.zst` and `.xz` select the
[optional zstd and xz](design-compression-libs.md), which fail as
unsupported in a build without their libraries,
and a container suffix (`.dmg`, `.vhd`, `.simg`) selects nothing but must
match the stack's container, so `-a -f disk.dmg -F fat32/gpt` is a syntax
error. A double suffix is matched outermost first: `disk.dmg.gz` selects
gzip and requires a `dmg` container. Without any of the three the output
is uncompressed, whatever its name says. A NAME the registry does not hold
is a syntax error; a registered compression whose library the build lacks
fails as unsupported. `--compress` or `-z` together with
`-a` is accepted only when the suffix names the same compression, and
`--compress` and `-z` naming different ones are a syntax error. `-a` with
`-f -` is a syntax error, since a stream has no name; `--compress` is how a
stream is compressed. On reading,
compression is decided by the layer design's
[step-1 rule](design-layers.md#peeling-order): `--compress=NAME` or `-z`
forces it, and when a file passes for both compressed and raw, a forced
compression resolves it as compressed and
an explicit `-F` as raw. Compressed and stream inputs are decoded into the
budgeted spool inside the worker. The command line names no codec itself
beyond `-z`'s: the compression `--compress`, `-z` or `-a` selects travels in the worker's
job record as its registered name, which the worker hands to the library's
write or image options, so a codec the registry gains reaches `-a`
through the suffix it declares, and `--compress` through its name, with no
change to the command line.

The [settings catalogue](design-settings.md) supplies setting identity,
representation, syntax, and applicability to the CLI and library. The CLI owns
argument handling, explicit-key repetition checks, syntax diagnostics, and
host path preparation; the catalogue performs no image or host-path I/O.

`-o key=value` is the repeatable typed setting, each key at most once. An
unqualified key always targets the filesystem: `oem=cp437` on read; on
create `sector` and `cluster` in bytes, and the QNX6 settings
`endian=little|big`, `uuid=` RFC 4122 text, `inodes=` a positive inode count
and `reserve=` a percentage up to 100, which every other format rejects
unless at their defaults; QNX4 shares `inodes=` as its `.inodes` slot count,
FAT12 and FAT16 take `root=` a positive root directory entry count,
and ImageFS shares `endian=` and adds `mountpoint=`, its stored mount path
of at most 1024 bytes, default `/`, with `mountpoint=` alone storing an
empty one; its writer takes no boot setting, since it writes
[nonbootable bodies](design-ifs.md#bootable-bodies) only. APFS shares
`uuid=` for its volume UUID and adds `case=insensitive|sensitive` on create.
On read, APFS takes `volume=`, a slot index selecting one volume of a
container that holds several, and `snapshot=`, a snapshot name, as the
[APFS design](design-apfs.md#volume-selection) states. Every other format
refuses these settings; a read setting is refused once detection has chosen
the format. ImageFS has no free
space, so `-s` and `--free-space` fail with it as settings the format
cannot honor, and `-e` is its only time setting.
ISO 9660 takes `level=1|2|3`, the primary hierarchy's interchange level,
default 3; `boot=PATH` and `efiboot=PATH`, image paths of files in the
tree being created that become El Torito BIOS and EFI boot images, never
host paths; and `bootload=N`, the BIOS entry's 512-byte load sectors,
default 4, only with `boot=`. Its `-s` pads the volume with zero blocks,
and `--free-space` fails, since the format has no free space. On read,
`view=auto|rockridge|joliet|primary` chooses the
[view](design-iso9660.md#view-selection); it applies to whatever ISO 9660
volume detection finds, and an image of another format refuses a
non-default `view`, as the [settings catalogue](design-settings.md)
states.
A qualified key targets one layer: `-o gpt:type=esp`,
`-o gpt:name=EFI`, `-o dmg:compress=none`, `-o gzip:level=9`. A qualifier
naming a layer absent from the stack, or a compression not selected, fails;
fsimg never guesses the target of a bare key. A
[file setting](design-layers.md#file-settings-and-sections) takes a host
path (`-o ifsboot:startup=startup.bin`, `-o ifsboot:prefix=bios-prefix.bin`,
the raw loader bytes),
resolved against the working directory like `-f`, never against `-C`. The
coordinator opens it before the worker starts, with the same no-follow,
nonblocking, no-controlling-terminal open as an operand, requires a
regular file, and passes the read-only descriptor to the worker in a slot
of its own; the worker receives no name, the library checks the length
rule the layer's profile states, and the sandbox admits the descriptor as
it admits the image input. On read the layer must be
named in `-F`, because detection runs later inside the worker and every key
is validated first: `-F fat32/gpt -o gpt:sector=4096` is the remedy for a
disk carrying GPT headers at both sector sizes, and `-o gpt:sector=4096`
alone fails. The layer key set is defined in the
[layer design](design-layers.md#layer-settings). Unknown keys and unsupported
values fail before any worker starts. Without `-R`, a QNX6 volume receives a
random version 4 UUID, every volume a random serial, and every scheme and
container a random [identifier](design-layers.md#identifiers), unless the
setting is given.

Read-side selection:

| Option | Meaning |
| --- | --- |
| `--partition=N|name=TEXT|type=NAME` | The partition holding the volume; the automatic choice and the selectors are the [layer design's](design-layers.md#partition-selection); an error on a disk without a partition scheme |
| `--offset=bytes`, `--length=bytes` | A manual volume slice within the unwrapped image, validated against its length; the length is positive; conflicts with `--partition` |

Members are image paths, relative to the image root (`EFI/BOOT`) or absolute
(`/EFI/BOOT`), both naming the same entry. A member is validated UTF-8,
matched by each format's component lookup rules including FAT case
equivalence and short aliases, and displayed with its image spelling.
Several members select the union of their subtrees; an entry selected twice
is processed once. No member means `/`; a member matching nothing fails. The
reader may scan preceding metadata to find a member but reads no unselected
payload and builds no whole-volume pathname table.

## Creation

Operands are host paths, as for tar: each is opened relative to the current
`-C` directory (the working directory until a `-C` is given; absolute
operands as given) and stored under its own name, so
`fsimg cf x.img -F fat32 rootfs` produces `/rootfs/...` and `-C rootfs .`
places rootfs's contents at the image root. `-C` must name an existing
directory, opened following symbolic links as tar's `-C` is; an existing
object of another kind, judged on the object followed, fails as
unsupported, and a path through a non-directory or a dangling or looping
link as an open failure. It applies to the operands after it, and may
repeat:
`-C esp . -C extra EFI`. Each `-C` is resolved against the working
directory, not against the previous `-C`, so the operand list reads the same
in any order. A `-C` that applies to no operand (two in a row, or one at the
end) is a syntax error. An operand may be a regular file. An operand
`@file` names an mtree(5) manifest, opened relative to the `-C` directory
in effect, whose entries are rooted at the image root and whose host bytes
come from beneath that directory; before any `-C` a manifest has no content
directory and accepts only generated files, as the
[manifest design](design-mtree.md#command-line) specifies; `./@name` names
a host object beginning with `@`, and `@` is recognized after `--` too.

The image name of an operand is its path with empty and `.` components
removed; a `..` component is a syntax error, not stripped. An operand with
no remaining components contributes the directory's contents at the image
root and no entry of its own. A multi-component operand `a/b/c` stores `c`
as `/a/b/c` beneath synthesized parents `/a` and `/a/b`: directory entries
with no valid metadata field, added like any programmatic directory, so each
format's documented defaults for absent fields apply (QNX6: mode `0755`,
uid and gid zero, its timestamp fallback; `-R`: the epoch) and host
directories the user did not name never influence the image.

Operands merge by the [merging rule](design-sources.md#merging-roots):
directories supplied by several operands become one directory with the union
of their children; any other collision fails planning; a directory's
metadata comes from the first operand in command-line order that scanned it,
and a synthesized parent never outranks a scanned directory. Naming the same
tree twice fails on its first file.

The coordinator opens every operand before the worker starts, with the same
no-follow, nonblocking, no-controlling-terminal open as any scanned object,
so a symbolic link operand fails, and passes the opened roots to the worker
as its only source authority. Scanning rules apply per root: type checked
before reading, special files rejected, and mount points beneath it crossed
as tar crosses them.
The creation sandbox has broader source-root authority than the
image-reading sandbox; read-only access alone does not prevent opening
device nodes already beneath an authorized root, a device filesystem
mounted beneath it included, so adversarial sources needing device
exclusion use an isolated filesystem with device nodes disabled and no
descendant mount that allows them. This [profile limitation](design-security.md#source-root-profile)
is reported with creation capabilities.

| Option | Meaning |
| --- | --- |
| `-F stack` | Required stack |
| `-L label` | Volume label; default `FSIMG`, subject to format rules |
| `-M` | Minimize: choose the open geometry settings giving the smallest volume; conflicts with `-s` |
| `-R` | Reproducible image bytes |
| `-e epoch` | Timestamp epoch for `-R`; default Unix second 315532800 (1980-01-01 UTC); requires `-R` |
| `-n` | Collect, validate and plan; report the plan and create nothing; `-f` is then optional |
| `-p` | Import host owner, group and permission bits as optional metadata |
| `-s bytes` | Positive exact logical volume size; omission selects automatic sizing |
| `--free-space=bytes` | Minimum free space after metadata and allocation rounding, in bytes; distinct from the QNX6 `-o reserve=` percentage, which is why the option is not called reserve |
| `--compress=NAME`, `-z`, `-a`, `-o key=value` | Compression and typed settings, as above |
| `-W` | Verify the staged result before publication; `-d` adds payload reads; a syntax error with `-f -` |

Reproducible mode normalizes timestamps to the selected epoch, uses defined
stable identifiers and fixes ordering and padding. It does not read the host
timezone, locale or `SOURCE_DATE_EPOCH` implicitly. Without `-R`, the
declared source timestamp policy and generated identifiers apply. Geometry
candidates and format defaults appear in the plan.

Host import includes regular files, directories and symbolic links, content,
hierarchy and representable timestamps, except that the Linux sandbox cannot
read a host link ([security](design-security.md#linux)), so a confined Linux
import refuses one; `-p` adds owner, group and permission bits. Special
files are rejected. A format that cannot store a kind fails planning; a
field it cannot store is an optional loss counted in the usual notice, so
FAT and exFAT advertise no POSIX fidelity and `-p` never changes their image
bytes. The default label applies only to formats with a label. Hardlinked
host files are copied as independent files.

The output name must not already exist. Planning completes before the image
file is created; payloads are written once to a staged image on the
destination filesystem, and publication is atomic and without replacement
where the supported primitive establishes that guarantee. There is no
force-overwrite flag. A failed operation reports whether a staged or
published output exists and whether durability is uncertain.

`cW` runs the ordinary read-side verification on the staged image, in a
second worker under the image-reading profile, after the creation worker
exits and before publication. A failed verification discards the staged
result and publishes nothing; the error names the failing layer and says so.
The capability record and the summary's `sandbox` field describe the
creation worker; each worker still warns separately when it runs
unconfined.
`cW` with `-f -` is a syntax error: a stream leaves no complete artefact for
the second worker to open. The verification reads a compressed image
through the spool, and a compression of a bare disk decodes to exactly
that disk, so `cW` refuses such a planned disk larger than `-l spool`
as soon as the creation worker reports it, without waiting for the
creation to finish. Under a
container the spool holds the container's file, which can be shorter than
the disk, so planning refuses nothing and the verification's own spool
reservation is the check.

`-f -` streams the final bytes to stdout, and is refused when stdout is a
terminal, as `gzip(1)` refuses to write compressed data to one. The layer
design specifies how the staged result becomes a sequential stream with its
container and compression. A stream has no publication step and no atomic
outcome: a
failure after bytes were written reports incomplete output through the exit
status and stderr. `--json` is rejected with `cf -`, because stdout carries
the image.

`-n` may use metadata scratch but creates no output and reads no payloads.
It reports logical size, allocation geometry, payload, metadata and free
bytes, entry count and known resource estimates with estimates labeled. It
cannot certify future source stability or disk space.

## Extraction

`-C dir` is always an existing directory, opened following symbolic links
as tar's `-C` is unless `-E` is given; an existing object of another kind,
judged on the object followed, fails as unsupported, and a path through a
non-directory as an open failure. Extraction creates exactly one
fresh directory inside it, named after the image stem, and extracts beneath
that: `fsimg xf boot.img` extracts into `./boot/`, `fsimg xf boot.img -C /tmp`
into `/tmp/boot/`. This is GNU tar's `--one-top-level`, made the default
because a directory fsimg created itself is the one it can prove empty and
safe from other writers.
The coordinator creates it with one `mkdirat` with mode 0700 beneath the
opened `-C` descriptor and reopens it no-follow; an existing name of any kind
fails, and there is no merge. The worker receives the fresh directory's
descriptor, never the parent's.

The stem is the basename of the `-f` path with suffixes stripped from the
outside in, each at most once, matched ASCII case-insensitively: one
compression suffix, then one container suffix, each as the
[layer registry](design-layers.md#layer-contract) declares it (`.gz`; `.dmg`,
`.vhd`, `.simg`), then one generic image suffix (`.img`, `.raw`, `.bin`,
`.dd`), the only suffix list the CLI keeps itself. `disk.img.gz` yields
`disk`, `system.dmg` yields `system`, `rootfs.vhd.img` yields `rootfs.vhd`.
If nothing was stripped, the input is `-f -`, or the stem is empty, `.`,
`..` or otherwise not one valid host name, fsimg invents no name: extraction
then requires `-E`, and the error says so.

`-E` extracts directly into the `-C` directory, which must be empty and
private. The coordinator opens it no-follow and establishes on that
descriptor alone that it is a directory owned by the effective user, not
group or other writable, empty, and carrying no non-trivial access control
list of any type the platform can report; the
[security specification](design-security.md#safe-extraction) defines the
checks, states why they keep other writers out while read and search bits
for others are allowed, applies the same ACL rule to the fresh directory, and states
why fsimg never edits an ACL. The worker then receives this descriptor
exactly as it would a fresh one, and extraction proceeds unchanged: every
entry is created exclusively and no-follow beneath a verified parent, so an
object another process adds concurrently is a collision failure, never an
escape.
A non-empty `-C` with `-E` fails; merging into a populated tree is not
possible by any option.

| Option | Meaning |
| --- | --- |
| `-C dir` | Parent of the fresh destination; default the working directory |
| `-E` | Extract directly into the empty private `-C` directory |
| `-k` | Skip entries of unsupported kinds and report their count |
| `--mtree=FILE` | Also write an mtree(5) manifest of the extraction to the new `FILE` |
| `-O` | Write one member's logical bytes to stdout instead of extracting |

Each member keeps its path below the image root, including necessary parent
directories. The CLI uses the
[safe content profile](design-security.md#safe-extraction): private
directories and files, regular content only, no links, no privileged
metadata, no existing-tree merge and no host timestamp restoration.
Inspection still exposes image metadata. An entry of another kind, such as a
symbolic link, fails the extraction unless `-k` skips it; skipped entries are
counted in a warning and in the JSON summary. An entry the reader omits for
a name outside its profile, under the
[omitted-names contract](design-directories.md#omitted-names), fails the
extraction the same way, named in the escaped rendering, unless `-k` omits
it; omitted entries have their own count, `omitted_entries`, reported after
`t`, `x` and `W` alike by the warning `N entries with names outside the
profile were omitted` and the JSON summary field of that name. No force
flag, lossy name
rewriting or sandbox opt-out weakens the path and file-type checks.
`--mtree=FILE` records what the safe profile cannot restore, skipped entries
included, in a manifest the coordinator publishes beside the tree, as the
[manifest design](design-mtree.md#extraction-writing-a-manifest) specifies.

Logical output bytes, including holes, count against the output quota.
Sparse files are used where possible. Success means all selected content and
required finalization completed. On error a private partial destination can
remain; the diagnostic identifies it. There is no all-or-nothing directory
publication or resume promise. Final report delivery is part of command
completion: if that delivery alone fails, extracted content may already be
complete, while the command still conservatively reports incomplete output.

`x -O` takes exactly one member, a regular file or a symbolic link (whose
bytes are its target), and writes its logical bytes to stdout, including
explicit zeros for logical zero ranges. The content is raw; terminal
escaping would corrupt it. `-O` rejects `--json`, `--mtree`, `-C`, `-E`
and `-k`. A
broken output pipe or a content error produces failure rather than a
truncated success result.

## Listing

`t` lists, in the reader's order, each selected member's own entry, a
directory as well as a file, and by default the member's whole subtree.
The directories leading to a member are listed too, before it, because `x`
of the same members creates them: `t` lists exactly the entries `x` visits,
each once, and `x --mtree` records every one. `x` creates each entry it
visits, or, for a kind it cannot create, refuses it or under `-k` skips it
and counts it in `skipped_entries`, not in the processed entries. The root
has no entry of its own, so no member, or `/`, lists the volume's contents.
Every listed entry counts in the summary.

`--no-recursion`, a `t` option, lists one level: each member's entry and,
for a directory member, its direct children only, after the directories
leading to the member; with no member it lists the root's direct children.
It differs from GNU tar's `--no-recursion`, which selects only each named
member itself, and every entry when no member is named. `-n` keeps its one
meaning, the create plan. The default listing has one
quoted, escaped image path per line, a symbolic link marked with `@` and a
FIFO with `|` after the name, and no completion trailer.

`-v` gives the long listing, one line per entry:

| Column | Content |
| --- | --- |
| mode | Kind character `-`, `d`, `l` or `p`, then nine permission characters when the entry has a valid mode and nine `?` when the format stores none |
| size | Logical content length in bytes for regular files and symbolic links; `-` for directories and FIFOs |
| mtime | `YYYY-MM-DD HH:MM:SS` from the modification timestamp: UTC for UTC and known-offset instants, the recorded calendar fields for an unknown-offset time; `-` when absent |
| name | As in the default listing |

The long listing consults no locale or host timezone, so it is the same on
every host. `--json` entry records carry the exact fields, basis and
precision; `-v` with `--json` affects only stderr.

## Verification, information and formats

`W` checks the layers around the volume, then the supported profile's
complete structure; `-d` additionally reads initialized payload bytes,
decoding every container chunk for the container's checksums, and logical
zero tails need no backing read. A container field with no known rule,
such as `hbcifs`'s `0x14` and `0x1c`, is named beside the container in
the result, and the `--json` verify record is then `"complete":false` with
those fields in `unverified`; every other check passed, so the exit status
is that of a passing verification.
Verification reports unsupported critical features, corruption, incomplete
work and resource limits distinctly, and never repairs an image. One human
result is emitted only after successful worker exit and finalization,
stating structural or initialized-payload scope. Entries the reader omitted
for a name outside its profile are reported by the omitted-entry warning
and summary field next to a complete result, never as corruption; for
ImageFS the result also prints the `shadowed` detail when records are
shadowed, the `omitted_references` detail when a boot or script
reference resolves to an omitted node, and the `shadowed_references`
detail when a shadowed record carries a boot or script reference's inode
word.

A byte range (`--offset`, `--length`) never reads a partition table, so it
conflicts with a scheme named in `-F` as it does with `--partition`; a
syntax error reports the combination.

`i` reports, in order, the detected or forced stack in `-F` syntax with the
detected compression; when the stack has a container, the container with
the length of the disk it holds, its chunk count, the encodings its chunks
use, its identifier and its header fields, one `name: value` line each with
the value in hexadecimal and a `container_details` object in the `--json`
stack record; when the stack has a scheme, a partition-table summary
with each partition's index, type, name, offset, length and whether it holds
a supported filesystem, marking the selected one; then the volume: format and
profile, volume and allocation geometry, label, mountpoint and volume UUID
when the format has them, time basis, implemented capabilities, validation
scope, and the volume's named details in the order the driver filled them,
one `name: value` line each in the human form and one `details` object
keyed by name in the `--json` volume record. When no
partition is selected and none is unambiguous, `i` still succeeds with the
stack and table and reports the volume section as unselected, so the user
can choose; every other mode fails. `i` claims no verification and no
free-space scan; counts requiring traversal are marked unknown rather than
triggering an undisclosed scan.

`--formats` reports compiled capabilities: filesystem formats with their
read, write and verify support, profiles, geometry restrictions and metadata
fidelity; partition schemes; containers with the chunk encodings they read;
containers and compressions that use zlib with its version string, since
compressed bytes are reproducible only for one zlib; and
sandbox backends. Availability distinguishes a compiled backend from a usable
runtime facility, so a sandbox backend appears as compiled and probed. `-v`
adds detailed restrictions on stderr; `--json` emits one `format` record per
entry with a `kind` of
`filesystem`, `scheme`, `container` or `compression`, and a `capabilities`
record. Formatting a volume and booting a machine from it are separate
capabilities; fsimg does not promise to install boot loaders.

## Output, diagnostics and exit status

Human output is concise and terminal-safe; names and control characters are
escaped unambiguously. Diagnostics and progress go to stderr. Payload output
and data records go to stdout, which carries exactly one thing per
invocation: an image stream, a member's bytes, a listing, a plan, a report
or JSON records. Progress has bounded frequency and can be disabled; it
never accumulates one record per processed file in memory.

Successful creation and extraction are silent by default. `i` uses aligned
labels and binary byte units, identifies unknown counts without an implicit
scan, and states header-only validation. Creation plans appear with `-n` on
stdout or with `-vv` on stderr. Verbose completion details use stderr. Human
success is determined by exit status; the mandatory machine completion
record belongs to `--json`.

Human errors use `fsimg: explanation`, optionally with a quoted pathname and
an OS error description. Prose has no enclosing quotes. Empty internal
fields, numeric errno values and generic status prefixes do not appear.
Resource failures identify the resource, limit and observed demand. Actual
cleanup failures, retained output and scratch paths, incomplete output,
published output and uncertain durability remain visible in plain language.
Truncated paths and messages are labeled; terminal escaping applies to prose
as well as names. The CLI's escaping follows the field's type, not a list
of producers. An entry path, the one unrendered path the library emits, is
escaped in full:
`"`, backslashes and control scalars. Every other library path, in an
error record, a loss notice or a verification finding, arrives already
rendered by the
[one rendering function](design-directories.md#omitted-names), every
backslash as `\\` and, in an omitted component, every byte outside `0x20`
to `0x7e` as `\xNN`; the CLI escapes such a field without touching its
backslashes, so the library's escapes survive unchanged, while `"` and
control scalars, including the C1 and bidi formatting characters a
delivered name may carry, are still escaped as `\"` and `\uXXXX`. The
result stays unambiguous because every backslash sequence has one of these
two origins, and a delivered backslash shows as `\\` in a listing and in an
error alike. In JSON the rendered text is the string's value under ordinary
JSON encoding, so an entry-path value holds a raw backslash while an
error-path value holds the rendering's two. FAT layout failures identify
the rejected constraint when all candidates fail for the same reason;
otherwise they describe the
conflicting requested settings without guessing a remedy or an uncomputed
minimum image size.

Optional metadata loss on success produces one final notice counting
affected fields, not files. Verbose examples name fields and are explicitly
sampled; truncated path excerpts are labeled. Samples never establish exact
totals. Failed commands report the primary error and required output and
cleanup consequences without an additional metadata-loss notice. Verbose or
JSON samples can precede failure. FAT sizing errors explain the limiting
constraint; automatic-size sentinels and unrequested free-space reserves do
not appear as zero-byte quantities. Sandbox-unavailable and disabled
warnings remain visible before processing regardless of verbosity or JSON
mode. Source profile limitations remain documented and appear in verbose
creation and capability details.

`--json` writes newline-delimited JSON records with a schema version and
`type`. Entry records stream individually, followed by a completion summary.
There is no JSON array proportional to entry count. Final records include
validation scope, counters, selected limits, metadata-loss, skipped-entry
and omitted-entry counts and sandbox status. An error emits a bounded error
record when the
stream is usable; absence of a successful completion summary means
incomplete output.

Exit status is 0 for complete success, 1 for operational failure and 2 for
invalid command syntax. Corruption, unsupported required features, partial
output and uncertain durability are failures. A sandbox-unavailable warning
under `auto` alone does not make otherwise successful work fail. Signals and
cancellation stop the worker, suppress publication and report incomplete
output when possible.

## Examples

```sh
fsimg cf boot.img -F fat32 -L BOOT -s 64M -C esp .
fsimg cRf data.img -F exfat --free-space=100G -m 128M -T /scratch rootfs extra
fsimg cWf disk.vhd -F exfat/gpt/vhd -o gpt:type=esp -C esp . -C extra EFI
fsimg cf disk.img.gz -F fat32/mbr -a -C esp .
fsimg cf system.dmg -F fat32/dmg -o dmg:compress=none -C esp .
fsimg cn -F exfat -R -m 64M -T /scratch rootfs
fsimg cf - -F fat32 rootfs | ssh host 'cat > boot.img'
fsimg if disk.img
fsimg tvf disk.img
fsimg tf disk.img --no-recursion /EFI
fsimg tf disk.img --json --partition=2 /EFI
fsimg tf odd.bin -F fat32/-/-
fsimg xf boot.img                      # into ./boot/
fsimg xf boot.img -C /tmp EFI/BOOT     # into /tmp/boot/EFI/BOOT
fsimg xEf boot.img -C ./empty-dir
fsimg xOf boot.img /EFI/BOOT/BOOTX64.EFI > BOOTX64.EFI
fsimg Wdf disk.img -S required
fsimg if disk.img --offset=1M --length=1G
fsimg --formats --json
curl -s https://example.org/disk.img.gz | fsimg tf -
```

## Coordinator and streaming representation

The coordinator parses trusted command operands, prepares private
capabilities and supervises its same-executable workers before publishing an
image. It never parses image bytes. The existing output relay owns every
potentially blocking stdout and stderr write, including help, capability
records, errors, final summaries, a streamed image and a member's raw bytes;
an output-only producer reuses the same relay queue, poll loop,
confirmed-byte accounting, signal handling and absolute monotonic deadline.
The [architecture](architecture.md#bounded-resources) owns the relay and its
ledger.

Command parsing owns the bundle rewrite, singleton and key-duplicate
detection, stack and setting validation and operand normalization. Numeric
parsing uses checked decimal accumulation and explicit binary byte suffixes.
Only known modes, options, stacks and settings reach descriptor preparation.

Read operations share the existing worker selector. A named `-f` input is
a read-only regular file or a finite disk device admitted by the
[device-input contract](design-device-input.md). The coordinator opens it
and prepares its fixed length and transfer quantum before worker startup;
the worker adopts that description without geometry queries. Named FIFOs,
sockets, terminals and other special files are refused. File settings keep
their regular-file requirement. For `-f -` the coordinator refuses a standard
input that is a terminal and otherwise pumps standard input into a relay pipe
byte for byte, through the input relay the
[architecture](architecture.md#bounded-resources) defines beside the output
relay, which interprets nothing it carries; the worker decodes the stream
and fills the spool as the [layer design](design-layers.md#filling) states.
This stream path applies even when standard input is seekable. The worker
never sees a socket, a terminal or an unknown descriptor kind; its sandbox
input check consumes the same regular/device/relay description. Device
inputs use existing partition and byte-range selection, require permission
to read the endpoint, and carry the stable-media, physical-disjointness and
blocking-call limits in the device-input contract. They add no device output
mode or payload spool requirement for an uncompressed positional image.

Create dry runs supply the source roots and optional scratch, never output
authority. Normal creation prepares a fresh private staging directory on the
final filesystem; its worker creates the image there only after planning.
Only the coordinator publishes, after a validated completion, successful
worker exit and, for `cW`, successful exit of the verification worker.
Extraction prepares the fresh stem directory, or checks the `-E` directory,
and retains it with an explicit incomplete outcome on failure.

The coordinator is the sole creator of the two
[handed-over files](design-layers.md#handed-over-files), each made
exclusively and unlinked before the worker starts, and places them where the
[storage design](design-storage.md#resource-model) says: the spool in the
private scratch tree, for every read-side worker, since the coordinator
cannot know whether the input will need it; the raw intermediate beside the
staged output in the staging directory when the output is a file, and in the
private scratch tree when it is a stream. With `-T -` there is no scratch
tree and no spool, and the coordinator reports a worker that needed one as
gzip input refused under `-T -`. A create's
[file settings](design-layers.md#file-settings-and-sections) are
read-only regular files the coordinator opened from the `-o` paths, one
descriptor slot per key, absent as `-1`; the worker wraps each in a
descriptor adapter and passes it to the library as the setting's I/O
object. The worker receives descriptors, never
names; its authority over names is the
[mapping](design-security.md#coordinator-and-worker) in the security
design.

Scratch is a separate fresh private tree beneath `-T`, `TMPDIR` or `/tmp`.
It is never an output ancestor or descendant. Cleanup never recursively
follows worker or image names. Scratch cleanup uses descriptor-relative
no-follow operations on coordinator-created trees after children exit.
Cleanup failures preserve the primary error and identify retained private
output, manifest and scratch locations.

Every private directory the coordinator creates, the staging directory of an
image or of an `x --mtree` manifest, the scratch tree and a fresh stem
destination, passes the destination's [rule against other
writers](design-security.md#safe-extraction) on its opened descriptor before
anything is put in it. A staged output is one record in the coordinator: its
final name and parent, its private directory and staged file, whether it was
published, and what remains of it, which the final report reads for the
publication state, the published name and the retained path. A staged image
and a staged manifest differ only in who creates the staged file and how its
length is known.

Output formatting retains one 128 KiB span, one small UTF-8 carry, the
current entry metadata and fixed result and counter records. Name frames
stream within JSON strings; no pathname or listing tree proportional to
entries is retained, and valid UTF-8 crosses name-frame boundaries intact.
An entry record yields a line by job: a listing's on stdout, a verbose
creation's or extraction's on stderr, a member output's none. Its
admission and the count it must agree with are the
[protocol's](design-extraction-worker.md#process-boundary-and-protocol)
rules; a creation or extraction not asked for names that sends one fails
as a forged worker does.
Human names escape controls, backslashes and bidi formatting unambiguously.
JSON uses valid UTF-8, escapes JSON controls and quotes, and encodes
untrusted invalid diagnostic bytes visibly.

Schema version 1 records use numeric schema=1 and string type, of which
there are stack, partition, info, plan, entry, verify, format, capabilities,
loss, progress, error and summary. A read mode's stack record precedes its
info record: the stack text, compression, scheme, disk size, sector size,
disk GUID or MBR identifier, the selected partition's slot and whether a
volume was selected, with the reason when none was. `i` follows it with one
partition record per used slot: slot, type name, type GUID, unique GUID and
name (GPT) or type byte and active flag (MBR), offset, length, the
filesystem claim and format, and whether it is selected. A creation whose
stack names a scheme reports the planned disk the same way before its plan
record: a stack record and the volume's partition record, with every
identifier the plan derived. A verify record
also names the verified scheme, the partitions whose bounds it checked and
the selected slot, each null when the selection read no table. Byte
quantities, counts, Unix seconds and nanosecond precision are decimal
strings with stated units; bounded kind and flag fields, nanoseconds, offset
minutes, geometry sizes, enumerated size arrays and the UTF-16 name-unit
limit are JSON integers; unknown values are null; a
timestamp includes basis and resolution. Progress and sampled loss details
use stderr; data, summary and error records use stdout. Entry records carry
complete=true only after all name frames and ENTRY_END validate; an error
after a partial entry first closes that record as incomplete, then emits an
error record, and no successful summary follows. Header, selected-entry,
structural and payload-read scopes remain separate. Capability records
distinguish the static format profile and compiled sandbox backend from
per-operation readiness; an unprobed runtime is unknown.

The command implementation separates parsing (options.c), bounded text
formatting (display.c), capability preparation and publication (main.c and
paths.c), and the existing worker, protocol and sandbox owners. The command
links the same core objects statically so fresh worker execution needs no
loader environment. The public shared and static library remains
independently installed and tested.

The operation deadline and the post-failure diagnostic grace are the
[architecture's](architecture.md#bounded-resources).
Scratch cleanup removes at most 4,096 flat entries and never descends into a
worker-created directory. A retained scratch directory or staging object is
reported explicitly.

`-l output` limits logical file bytes, including holes, rather than escaped
listing text. Summaries report queued text and confirmed stream bytes; every
reported ledger counter is a snapshot taken before its own record is queued,
so it excludes that record. Progress emits only when an entries, bytes or
work counter reaches a new power-of-two milestone, in addition to its
frequency limit; it therefore needs at most 193 records per operation even
for gigantic payloads. Suppressing redundant progress never stops
processing.
