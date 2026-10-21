# Manifests: creating from mtree(5) and writing it on extraction

## Purpose and scope

`fsimg c` builds an image from an mtree(5) manifest, alone or merged with
host trees, and `fsimg x` writes a manifest beside the tree it extracts. The
manifest supplies the hierarchy, entry kinds, names, owner, mode,
modification time, link targets and SHA-256 digests; the bytes of a regular
file come from a host file beneath a content directory, as with FreeBSD
`makefs -F` and a release `METALOG`, or are generated from the manifest
alone through namespaced `fsimg.` keywords. Nothing needs privileges: a
manifest that says `uid=0 mode=4755` produces exactly that in a QNX6 image
without any host object carrying it.

The design serves two consumers at once. A user rebuilds an image whose
metadata and link entries the safe extractor cannot restore on the host: `x
--mtree` records them, `c @manifest` reads them back, and the pair round-trips
what the format stores. The [specification-driven test
harness](design-spec-testing.md) drives `c` with a manifest so that a case's
names are arbitrary bytes, including names the host filesystem cannot hold,
and its file contents are generated, so no source tree touches the disk.

The grammar is the mtree(5) that FreeBSD, NetBSD and macOS document, read as
untrusted input under the same bounds as an image parser. Keywords fsimg
cannot carry are refused, never ignored. The `fsimg.` keywords are refused by
`mtree(8)` and bsdtar (both report an unknown keyword),
which is acceptable: a manifest with generated content is fsimg's own
input, and the manifest fsimg writes on extraction uses only standard
keywords so that other tools read it.

## What the code does today

The writer collects entries from two sources, as the [sources
design](design-sources.md) states: `fsimg_writer_add_tree()` scans a host
root descriptor and `fsimg_writer_add()` takes a caller's entry with a zero
or provider source. The scanner (`lib/source/scan.c`, `scan_entry()`)
records each file or link as one catalog entry plus one host source record
holding the root ID, device, inode, length, modification and change times
and the relative path; reopening (`lib/source/source.c`,
`open_host_source()`) walks that path one component at a time beneath the
root, no-follow, and compares the identity before and after copying. The
catalog sorts entries by path and resolves equal paths by rank
(`lib/write/catalog.c`). Owner and mode reach the catalog only under
`FSIMG_WRITE_IMPORT_POSIX`. The CLI worker receives one opened root per
create operand and nothing it could resolve a pathname against. The safe
extractor (`lib/extract/extract.c`) creates files and directories with private
modes, restores no owner, mode or change time, and creates no links or
FIFOs, so an extracted QNX6 tree loses exactly the fields a manifest can
carry. The manifest parser, `lib/source/mtree.c` (`fsimg_mtree_parse()`),
implements the [grammar](#grammar) and the parser's bounds: it reads a
manifest through a read callback, one bounded logical line at a time, and
hands each entry line to an entry callback as a decoded, validated entry
(image path, kind, size, owner, mode, time, link target, `contents=` path,
digest, content kind, seed and holes). `fsimg_writer_add_manifest()`
(`lib/source/manifest.c`) is the [source root](#a-manifest-is-a-source-root)
built on it: it registers the root, feeds the parser from the manifest
descriptor, and collects every entry. Generated content
(`lib/source/generated.c`) is the `GENERATED` storage class, which the
source cursor serves and the catalog accepts on scanned records only;
host contents are walked by the same component walk reopening uses
(`fsimg_source_walk()`), and their digests are checked by the cursor during
the copy. Digests claimed for generated content are kept, with their line
and image path, in a writer stream that `fsimg_writer_plan()` checks once
the image has accepted every size (`fsimg_manifest_check_digests()`).
The CLI does not yet accept a manifest operand and the extractor writes no
manifest; the [command line](#command-line) and
[extraction](#extraction-writing-a-manifest) sections describe the target.
The SHA-256 implementation is the one [Digests](#digests) describes,
`lib/core/sha256.c`.

## A manifest is a source root

A manifest is the third kind of source root, beside a host directory and a
host regular file: an opened manifest file paired with an opened content
directory. The manifest parser is a second scanner. It reads the manifest
once, line by line, and for every entry line appends exactly what the host
scanner appends for a host object: one catalog entry through
`fsimg_writer_collect()` and, for a file or link, one source record in the
host-source stream. Everything after collection is unchanged and shared:
catalog preparation, the [merging rule](design-sources.md#merging-roots),
the planner's capability check and loss policy, source reopening with
identity checks, the emission copy, publication. Nothing downstream can tell
a manifest entry from a scanned one, so no planner, driver, extractor or
CLI formatter learns about manifests. The one thing a manifest leaves
beside its records is the digests it claims for generated content, which
the writer checks once planning has finished, as [Digests](#digests)
explains; no driver sees them.

The requirements follow from this one construction:

* **Merging with host-tree operands and other manifests.** A manifest root
  has a rank like every other operand: its records rank as scanned records
  at the manifest root's registration order, its operand's position.
  Directories it shares with a host tree or another manifest become one
  directory with the union of the children and the metadata of the first
  operand that supplies it, and a file it shares with another root is a
  collision that fails planning naming the path. A release `METALOG` and
  its tree are one root, not two: the manifest names the files and the
  content directory supplies their bytes, so there is nothing to merge.
* **No privileges, no host metadata.** The manifest's keywords are the
  entry's metadata; a host file beneath the content directory contributes
  bytes and a length, never its owner, mode or times, whatever `-p` says.
  An absent keyword is an absent field, and the format's documented defaults
  apply exactly as for a programmatic entry.
* **Bounded metadata.** The parser holds one logical line, one current
  directory path and one record at a time; the manifest's entry count is
  charged to the entries limit as every collected entry is, and the
  catalog's scratch streams hold the result.
* **Reproducibility.** The catalog sorts, so the order of manifest lines does
  not change image bytes; the image is a function of the manifest bytes, the
  content bytes and the options.
* **The same worker and protocol.** The coordinator opens the manifest and
  its content directory and passes both descriptors as one root; the worker
  parses the manifest after acknowledgement, where it parses image bytes,
  under the creation profile.

Generated content is a private fourth storage class beside none, zero,
provider and host. Its source record holds the generator and its
parameters, and the writer's one source cursor serves it: `read_at()`
computes bytes and `next_range()` reports the holes. A manifest link is
generated storage too, its record holding the target the manifest states,
which the cursor serves as it serves every link target; a `zero` file is
the zero source and has no record. Collection and catalog validation
accept generated storage on a scanned record only; a programmatic record
still carries neither host nor generated storage, and a scanned one never
carries the provider. The public source kinds do not change.

## Command line

In mode `c` an operand beginning with `@` names a manifest, as it names an
archive for `tar(1)`: `fsimg cf disk.img -F qnx6 -C rootfs @METALOG`. The
word after `@` is opened relative to the `-C` directory in effect, with the
no-follow, nonblocking, no-controlling-terminal open every root gets, and
must be a regular file. That same `-C` directory, the working directory
until a `-C` is given, is opened as the manifest's candidate content
directory. It becomes the content directory only when an entry needs host
bytes: a manifest whose files are all generated reads nothing beneath it,
and its root is descriptorless, as the [library
interface](#library-interface) defines, so a directory that holds other
files, such as the harness's case directory with `out/` and `scratch/`,
serves as the working directory without becoming a source. No option spells
"no content directory". A host object whose own name begins with `@` is
named `./@name`. The `@` form is recognized in every operand position,
including after `--`. A manifest operand holds two descriptors, so it counts
twice against `fds`.

The manifest's entries are rooted at the image root; there is no prefix
form. A manifest that should land beneath `/a` says so in its paths.

| Option | Effect on a manifest operand |
| --- | --- |
| `-C dir` | Content directory and base of the manifest's own path |
| `-p` | None: owner and mode come from keywords, never from content files |
| `-R`, `-e` | As for every entry: every stored time becomes the epoch |
| `-n` | Parses the manifest, inspects content files and checks generated digests; reads no payload, so host digests are not checked |
| `-L`, `-M`, `-s`, `-z`, `-a`, `-o`, `-W`, `-f -` | Unchanged |

In mode `x`, `--mtree=FILE` writes a manifest describing the extracted
selection. `FILE` is a host path relative to the working directory, like
`-f`, created exclusively and published without replacement by the output
coordinator after a validated completion, the way a created image is;
failure leaves no `FILE`. With `-k`, skipped entries appear in the manifest
with their kind and metadata, which is the only record of them. `--mtree`
with `-O` is a syntax error.

## Library interface

```c
fsimg_status_t fsimg_writer_add_manifest(fsimg_writer_t *writer,
    int manifest_fd, int contents_fd, fsimg_error_t *error);
```

`manifest_fd` is an opened `O_RDONLY` regular file, borrowed for the call
and read through a duplicate from offset zero to the length it had when the
call began; a manifest whose identity, size or change metadata differ when
the parse ends fails with `FSIMG_ESOURCE_CHANGED`, and one that shrinks
while it is read fails as a short read. `contents_fd` is an opened
`O_RDONLY` directory, checked to be one when the call begins, duplicated and
registered as a root like the descriptor of `add_tree()`, consuming one
descriptor reservation, or `-1` when the manifest supplies no host bytes.
Either way the call registers one charged root record. With `-1` it is a
descriptorless root that holds no descriptor, identity or mount ID and
consumes no reservation, and the parser refuses at input, naming the line,
any entry that needs host bytes, so no host record names that root. With a
directory, the root takes the directory's identity and mount ID when the
first entry needing host bytes is parsed; if none does, the duplicate is
closed and the root stays descriptorless, so a manifest of generated files
behaves the same whether or not a directory was offered. The
manifest's records are scanned records whose order is that root's ID under
the [merging rule](design-sources.md#merging-roots), whatever their storage:
none, zero, host or generated. The call is valid in the COLLECTING state and is
terminal on failure, like `add_tree()`.

`fsimg_extract_options_t` gains `int manifest_fd`, `-1` from the
initializer: an opened, empty, writable regular file the caller created, to
which the extractor writes the manifest as it extracts. The library writes
and flushes it; publication under a name is the caller's. In the CLI that
caller is the output coordinator.

Generated sources have no public interface: a manifest is the way to ask for
them.

## Grammar

### Bytes, lines and words

The manifest is a byte sequence. A NUL byte anywhere refuses it. A physical
line ends at LF. An unescaped backslash ending a physical line, the last
of an odd run of backslashes since `\\` is an escaped one, joins the next
physical line: the backslash and LF are removed, so what follows is
separated by the whitespace the continuation line starts with. This is how
`mtree -c` wraps long entries. A comment never continues: `mtree -c`
writes a directory's raw path into the comment before it, so the comment
before a directory named `d\` ends in a backslash, and joining would
swallow the directory's own line. A logical line is bounded as
[Security and bounds](#security-and-bounds) states. The manifest ends with
an LF: input that stops inside a line, including after a continuation, is
refused, because a manifest cut short inside a line could otherwise lose
keywords and still parse.

Leading spaces and tabs are ignored. An empty line is ignored. A line whose
first word begins with `#` is a comment, so the `#mtree` and `#mtree v2.0`
signatures are comments and are not required. The joined text decides: a
continuation backslash at the start of a line, removed by the join, does
not hide a `#` after it, and that comment then ends at its own LF, so
there is one answer to whether a line is a comment. A comment may hold any byte
but NUL, since `mtree -c` writes raw directory paths into its comments.
Elsewhere `#` is refused: a `#` in a name is encoded `\043`, as `mtree -c`
and bsdtar write it.

Words are separated by spaces and tabs and consist of bytes `0x21` through
`0x7E`; any other byte in a word refuses the line. There is no quoting:
makefs's quoted words and `$VAR` substitution are not mtree(5) and are not
read. A line has a first word and up to 63 keyword words; the bound is
checked before the words are read, and no valid line reaches it, since
fewer keywords exist and none may repeat.

### Escapes

The first word, a `link=` value and a `contents=` value are vis(3)-encoded
and are decoded to bytes: `\ooo` with exactly three octal digits is the
byte with that value, and `\\` is a backslash. Any other byte after a
backslash refuses the line. `\000` decodes to NUL and so refuses the line.
This is the encoding mtree(5) specifies and the one `mtree -c`, bsdtar and
`x --mtree` produce; NetBSD's C-style forms (`\s`, `\n`) are not read.
Every other value is plain ASCII, and a backslash in it is refused.

### Lines

The first word decides the line:

* `/set key=value ...` sets defaults for the keywords it names; `/unset key
  ...` removes them, and `/unset all` removes every default. Any accepted
  keyword can be set. A name on `/unset` follows the keyword classes as on
  any line, and a repeat, `all` included, is refused. Any other word
  beginning with `/` is refused.
* `..` moves the current directory to its parent. It takes no keywords.
  The relative `.` entry at the image root opens the root as the current
  directory, so the `..` that `mtree -c` writes after the root's children
  closes it; a `..` with no open directory is refused.
* A word whose decoded bytes contain no `/` is a **relative entry**: a name
  in the current directory. `.` names the current directory itself. A
  relative entry of `type=dir` becomes the current directory for the lines
  that follow.
* A word whose decoded bytes contain a `/` is a **full entry**: components
  separated by `/`, relative to the image root, not to the current
  directory. A first component `.` is dropped, as in `./usr/bin`. A full
  entry never changes the current directory.

An entry's effective keywords are the current defaults overridden by the
line's own. A keyword that does not apply to the entry's kind and content,
by the "refused" rules of the value table below, is refused when the line
gives it and passed over when it is only a default, so one `/set` serves
entries of every kind. A keyword repeated on one line, an alias such as
`sha256` beside `sha256digest` included, is refused. The syntax words
`/set`, `/unset` and `..` are recognized before decoding; a decoded `.` is
the current directory and a decoded `..` is an invalid name. Both forms produce
one absolute image path, which collection validates as it validates every
path: UTF-8 scalar values only, no empty or `.` or `..` components, within
the path and depth limits. A manifest neither normalizes nor folds names,
so a name the host cannot hold, or two names a format treats as equal,
reach the catalog and the format's own rules exactly as if a host tree
held them; collection refuses a non-UTF-8 name as it refuses one from any
caller.

The current directory is one path buffer within the image-path limit; `..`
strips its last component. The `.` entry at the image root must be
`type=dir`; its other keywords are accepted and not stored, because the
root is implicit and carries no entry, the same as a host root scanned at
`/`.

A manifest describes each path once. A second line for the same path, in
either form, is a duplicate definition and fails preparation as a
collision (`FSIMG_EINVAL`, naming the path). The one exception falls out of the [merging
rule](design-sources.md#merging-roots), since both lines have the manifest's
rank: a directory repeated with the same effective keywords encodes equally
and merges with itself, as two host roots supplying the same directory do.
makefs refuses every repeat unless given `-D`; fsimg follows its own merging
rule rather than add a manifest-only exception to it. Every directory must be listed
before preparation needs it: a full entry whose parent no directory line
defines fails preparation with the catalog's missing-parent error.
`mtree -c`, bsdtar and `x --mtree` all list every directory; makefs and
bsdtar invent missing parents, and fsimg does not.

### Keywords

Each keyword is in one of four classes, decided by one table:

| Class | Keywords | Rule |
| --- | --- | --- |
| Carried | `type`, `uid`, `gid`, `mode`, `time`, `size`, `link`, `contents`, `sha256digest`, `sha256`, `fsimg.content`, `fsimg.seed`, `fsimg.holes` | Validated and used as below |
| About the host object, not the image | `nlink`, `inode`, `device`, `resdevice`, `tags`, `uname` and `gname` beside a numeric `uid` and `gid`, `flags=none`, `size` on a directory or FIFO | Validated as well-formed, then not consulted; `mtree -c` and bsdtar emit them by default |
| Unsupported | `uname` or `gname` without a numeric id (fsimg consults no user database), `flags` other than `none`, `cksum`, `md5`, `md5digest`, `sha1`, `sha1digest`, `rmd160`, `rmd160digest`, `ripemd160digest`, `sha384`, `sha384digest`, `sha512`, `sha512digest` (claims fsimg cannot verify), `optional`, `ignore`, `nochange` (mtree(8) run controls), `type=block`, `type=char`, `type=socket`, a symbolic `mode` | `FSIMG_EUNSUPPORTED` naming the keyword and line |
| Unknown | Everything else, including an undefined `fsimg.` name | `FSIMG_EINVAL` naming the keyword and line |

Values of the carried keywords:

| Keyword | Value | Meaning |
| --- | --- | --- |
| `type` | `dir`, `file`, `link`, `fifo` | The entry kind; required for every entry, usually through `/set` |
| `uid`, `gid` | Decimal, at most `4294967295` | `FSIMG_META_OWNER`, valid and optional; one catalog field, so both or neither |
| `mode` | Octal digits with or without a leading `0`, at most `07777`; bsdtar writes `644`, `mtree -c` writes `0644` | `FSIMG_META_MODE`, valid and optional |
| `time` | `seconds[.fraction]`: seconds a signed decimal within `int64_t`; the fraction one to nine digits, right-padded to nanoseconds | `FSIMG_META_MODIFIED`, UTC, precision 1 ns, valid and optional |
| `size` | Decimal, at most `18446744073709551615` | A file's logical length or a link's target length; checked against the bytes as [Content](#content) states; required for generated content, which has no host file to supply a length; a `/set` default applies to files, while a link checks only a size its own line gives, as `mtree -c` writes one |
| `link` | vis-encoded target, 1 through 1023 bytes, no NUL | Required with `type=link`, refused with any other type |
| `contents` | vis-encoded host path | A file's bytes; refused with any other type and with generated content |
| `sha256digest`, `sha256` | 64 hexadecimal digits, either case | The SHA-256 of the file's logical bytes; refused with any other type |
| `fsimg.content` | `host`, `zero`, `seeded`, `positional`; default `host` | How a file's bytes are obtained; refused with any other type |
| `fsimg.seed` | Decimal, at most `18446744073709551615` | The generator parameter; required by `seeded` and `positional`, refused otherwise |
| `fsimg.holes` | `offset:length[,offset:length]...`, decimal, at most 64 ranges | Zero runs inside generated content; refused with `host` and `zero` |

A host-object keyword is well-formed when `nlink` and `inode` are decimal
within 64 bits, `device` and `resdevice` are one to four comma-separated
fields of letters and digits (a number, or `native,1,4` as bsdtar writes),
and `tags`, `uname` and `gname` are any plain value. Every value is
nonempty.

Access, change and creation times, attributes and precision have no mtree
keyword and are absent; the owner decided that they are counted losses of
the round trip rather than extension keywords that other tools would refuse.

## Content

A file's bytes are exactly one of four things, chosen by `fsimg.content`.
The entry's `size` and the `sha256digest`, when given, describe the logical
bytes, with holes read as zeros.

### Host bytes

With `fsimg.content=host`, the bytes are a regular file beneath the content
directory: the `contents=` path when given, otherwise the entry's own image
path, which is makefs's rule and what makes a `METALOG` plus its tree one
root. The path is decoded bytes split on `/`; an absolute path or an empty,
`.` or `..` component is refused. The parser walks it exactly as reopening
walks a scanned path: each directory component opened `O_DIRECTORY |
O_NOFOLLOW`, each opened object checked for the root's device and mount
identity, the final component inspected without following a link, opened
no-follow only when it is a regular file, so a device or FIFO the manifest
names is never opened, and required again to be a regular file once open.
A symbolic link anywhere on the path fails as unsupported, as the scanner
fails one it cannot record; a directory, FIFO or special file at the
end fails as unsupported; a missing file, a path that continues through
a regular file, or a component longer than the content directory's file
system allows, is `FSIMG_EINVAL` with the OS error, since the manifest
named something that does not exist. The host
record stores the file's identity and the relative path, so reopening
during emission compares identity, size and change metadata as for a
scanned file and reports `FSIMG_ESOURCE_CHANGED` on a difference.

A `size` keyword must equal the file's length or the entry is refused;
without one the length is the size. The file's owner, mode and times are
never read into the entry.

### Generated bytes

The three generators are pure functions of the entry, so `-n` can check
them and two hosts produce the same bytes. `SHA-256` is FIPS 180-4;
`LE64(x)` is `x` as eight little-endian bytes.

* **`zero`**: the zero source, `FSIMG_SOURCE_ZERO`. No bytes exist, no
  storage backs them, `next_range()` reports one ZERO range, and `size` may
  be zero.
* **`seeded`** with `fsimg.seed=N`: the bytes are the stream of 32-byte
  blocks `B(i) = SHA-256(LE64(N) || LE64(i))` for `i = 0, 1, ...`, cut to
  `size`. The byte at offset `o` is byte `o mod 32` of `B(floor(o / 32))`,
  so any offset is computed from the seed alone. This is the definition
  the harness uses (`test/spec/lib/fsimg_spec/tree.rb`, `FileContent`),
  which packs the seed and the block index as `Q<Q<`.
* **`positional`** with `fsimg.seed=N`: the bytes are the stream of 16-byte
  blocks `P(j) = LE64(N) || LE64(16 * j)` for `j = 0, 1, ...`, cut to
  `size`. Every 16-byte-aligned group in the file names the file identity
  `N` and its own offset, so a cluster that lands in the wrong place, or a
  file that is served from another file's clusters, decodes to its origin.
  `N` is the file's identity; the manifest's author keeps it distinct per
  file when that matters.

`fsimg.holes` lists ranges that read as zero within seeded or positional
content. Each `offset:length` has a positive length, the ranges are in
increasing order and do not touch or overlap, and `offset + length` does
not exceed `size`, all under checked arithmetic. `read_at()` returns zeros
inside a hole, and `next_range()` reports the DATA and ZERO runs in order,
so the writer's zero-run handling in the [emission copy](design-emission.md#source-copy)
is exercised from a manifest alone.

### Digests

A `sha256digest` is a claim about the logical bytes, and fsimg checks every
claim it accepts. For generated content the bytes can be computed without
reading anything, but their size is only the manifest's word: hashing a
`size=2^63` file at input would be work no budget bounds, decided before
any format could refuse that size. So the import keeps each generated
file's claim, the line's number and byte offset and the entry's image path
beside the digest and the generator's parameters, in the writer's scratch
storage, and planning checks the claims after the driver, the disk layout
and the output limit have accepted the catalog. The bytes hashed then never
exceed the planned volume, at one unit of work per 64 KiB piece. That is a
bound on size, not on the cost of emission: emission skips zero runs, which
a fresh image never writes, while a digest covers every logical byte, so a
large `zero` or mostly-hole file in an automatically sized volume costs
hashing time in proportion to its size, which the work limit, cancellation
and the CLI's time limit bound. A mismatch fails planning with
`FSIMG_EINVAL` naming the line, its offset and the path exactly as a
refusal at input does, before the output is staged or written. A disagreement between the harness's generator and
fsimg's therefore fails before any byte is written. `-n` plans, so it
checks generated digests: it reads no payload, and the generated bytes are
computed, not read, so plan-only mode reports every refusal a run would
report except one that needs host bytes and the output limit's charge for
the raw copy an unencoded stream is written from. For host bytes the digest is checked
where the bytes are read, during the emission copy: the source cursor
hashes the bytes it serves in order from offset zero and `finish()`
compares once every byte was served, so a mismatch is
`FSIMG_ESOURCE_CHANGED` naming the path, the staged output is discarded and
nothing is published. Finishing a cursor asserts that it served the file:
one that served less than all of it in order is an internal fault
(`FSIMG_ECORRUPT`), never a silently skipped check, and the alias
preflight, which opens every host source only to compare identities,
closes its cursor instead of finishing it. Checking host digests at input
would read every payload twice and make `-n` read payloads, which the [CLI
contract](design-cli.md#creation) forbids.

The project has one SHA-256 implementation in `lib/core/`, used by the
generators, the digest checks and the extraction writer: FreeBSD's
`sys/crypto/sha2/sha256c.c` (BSD-2-Clause, Colin Percival), imported with its
copyright and license notice preserved, adapted to the project's build, to
hosts with a 32-bit `size_t` and to empty updates (the [implementation
references](../docs/references.md) list each change), and tested against the
FIPS 180-4 vectors.

## Metadata mapping

Every keyword becomes an existing catalog field or kind. The driver's
capability record and the [common loss policy](design-metadata.md#writer-policy)
then decide, unchanged: a field the format stores is encoded, a narrower
range or precision is an optional loss, a field it does not store is an
omission counted once per field, and a kind it lacks fails planning.

| Keyword | Catalog | FAT12/16/32 | exFAT | QNX6 | QNX4 |
| --- | --- | --- | --- | --- | --- |
| `type=dir`, `type=file` | kind | Stored | Stored | Stored | Stored |
| `type=link`, `type=fifo` | kind | Fails planning | Fails planning | Stored | Stored |
| `uid`, `gid` | `OWNER` | Counted loss | Counted loss | Stored | Stored within 16 bits, else a loss |
| `mode` | `MODE` | Counted loss | Counted loss | Stored | Stored |
| `time` | `MODIFIED` | Two-second wall time within 1980 through 2107, else a loss | Its stored precision, else a loss | Unsigned 32-bit seconds, else a loss | Unsigned 32-bit seconds, else a loss |
| absent `time`, `uid`, `gid`, `mode` | absent | Format default | Format default | 1980 fallback, uid 0, `0755`/`0644`/`0777` | Its documented defaults |

Under `-R` every stored time is the epoch, as for scanned entries, and a
manifest `time` has no effect on the bytes.

## Extraction: writing a manifest

The extractor writes one line per entry it visits, in the reader's order,
directories before their children, so the manifest is deterministic for a
given image and selection and needs no state beyond the current entry. The
form is the full form bsdtar writes, which FreeBSD and NetBSD `mtree(8)`
read; macOS `mtree(8)` reads only the relative form, a limitation the user
documentation states.

```text
#mtree v2.0
. type=dir mode=0755 uid=0 gid=0 time=946684800.000000000
./bin type=dir mode=0755 uid=0 gid=0 time=946684800.000000000
./bin/sh type=file mode=0555 uid=0 gid=0 time=946684800.000000000 size=1183848 sha256digest=...
./lib/libc.so type=link mode=0777 uid=0 gid=0 time=946684800.000000000 link=libc.so.7
./var/run/log type=fifo mode=0666 uid=0 gid=0 time=946684800.000000000
```

Each line has the path, `type`, then `mode`, `uid`, `gid` when their fields
are valid, `time` when the modification time is valid, `size` and
`sha256digest` for a file, and `link` for a symbolic link. Names and link
targets are encoded with `\ooo` for every byte outside `0x21` through `0x7E`
and for `\` and `#`, so the line is plain ASCII. `time` is nine fractional
digits. A wall-clock or offset modification time is written as the instant
the extractor would set on the host under the same `wall_offset_minutes`
interpretation, so the manifest and the host tree agree. The digest is
computed from the bytes the extractor copies, so it describes the host file
it wrote. Access, change and creation times and attributes are not written
and are the losses the extraction already counts. The root line carries the
root's fields when the reader reports any. With `-k`, a skipped entry is
written with everything but a digest.

The manifest's lines go to `manifest_fd` through the context's bounded I/O
in the order above; the extractor flushes and checks the close before it
reports completion. The CLI stages the file beside the image output's
staging, publishes it after the worker's validated completion and the
destination's finalization, without replacement, and removes it on any
failure. An existing `FILE` fails before the worker starts.

## Security and bounds

The manifest is untrusted input, parsed inside the worker after
acknowledgement, under the creation profile. The coordinator opens the
manifest file and the content directory as it opens every root and passes
descriptors, never names. The worker's only new act is to resolve relative
paths taken from the manifest beneath the content directory, which is the
component walk source reopening already performs; the content directory is
the `-C` directory the user named, the same authority an operand of `.`
grants, and the [source-root profile's](design-security.md#source-root-profile)
device limitation applies to it unchanged. A link target is bytes stored in
the image and is never opened or resolved. A `contents=` path never follows
a link and never leaves the content directory, because every component is
opened relative to the previous one with no-follow and `..` is refused as
text. The output and scratch identities are compared with every collected
host source before any sink mutation, as for scanned sources.

| Bound | Value | Failure |
| --- | --- | --- |
| Logical line | `8 * path limit + 4 * 1023 + 4096` bytes, charged to the memory budget: two encoded paths (the name and a `contents=` path), an encoded link and the other keywords | `FSIMG_ELIMIT`, resource `PATH` |
| Words per line | 64 | `FSIMG_EINVAL` |
| Keyword name | 32 bytes | `FSIMG_EINVAL` |
| Numeric value | 20 decimal or 22 octal digits, checked accumulation | `FSIMG_ERANGE` |
| Entries, depth, path | The context's limits, through collection | `FSIMG_ELIMIT` naming the resource |
| Holes | 64 ranges per entry | `FSIMG_EINVAL` |
| Generated digest checks | One per claiming file, in scratch storage; checked after planning, so the bytes hashed are bounded by the planned volume, each 64 KiB one unit of work | `FSIMG_ELIMIT`, resource `WORK`, naming the line |
| Manifest length | Not bounded separately: lines are streamed and each physical line, one that only continues included, is one unit of work, which also polls cancellation | `FSIMG_ELIMIT`, resource `WORK` |

All offsets, sizes, hole bounds and block indices use checked arithmetic.
The parser retains no per-entry heap object. `fuzz/mtree.c` feeds arbitrary
bytes to `fsimg_writer_add_manifest()` with no content directory and with a
fixed small one, under finite limits, and drives the generated sources'
`read_at()` and `next_range()` at random offsets, as the format fuzz targets
drive their parsers; `build-aux/fuzz.sh` runs it with the others.

## Failure results

| Condition | Status | Where |
| --- | --- | --- |
| NUL, a byte outside the word range, a bad escape, a word beginning with `/` other than `/set` and `/unset`, keywords on `..`, `..` with no open directory, input ending inside a line, a repeated keyword, an unknown keyword, a bare keyword, a malformed value, `link` or `contents` or a digest on the wrong type, an entry without `type`, a `type=link` without `link`, `uid` without `gid` or the reverse, generated content without `size`, `fsimg.seed` or `fsimg.holes` with the wrong content, a `size` that disagrees with a link target or the host file, a `contents=` path that is absolute or contains `..`, names a missing file or has a component too long for its file system | `FSIMG_EINVAL`, operation `SOURCE`; `offset` is the byte offset of the line, the message begins with its line number, `path` the entry's image path when one exists | Input |
| A keyword of the unsupported class, a symbolic link on a `contents=` path, a directory or special file at its end | `FSIMG_EUNSUPPORTED` | Input |
| A value beyond its numeric range | `FSIMG_ERANGE` | Input |
| A line, entry count, depth or path beyond its limit | `FSIMG_ELIMIT` naming the resource | Input |
| A mount crossing beneath the content directory | As the scanner reports it | Input |
| A duplicate path within the manifest, a missing parent, a file or link against another root's entry | The catalog's existing preparation failures, naming the path | Planning |
| A kind or field the format cannot store | `FSIMG_EUNSUPPORTED` under the loss policy | Planning |
| A generated digest mismatch, once the plan has accepted every size | `FSIMG_EINVAL`, operation `SOURCE`, naming the line, its offset and the path as at input | Planning |
| A content file changed, replaced or not matching its digest | `FSIMG_ESOURCE_CHANGED` | Emission |
| `--mtree` name already present, or its publication failed | The output coordinator's existing results | Extraction |

The error record names the line by byte offset and line number and, where
an entry exists, its path; a diagnostic never quotes the manifest's bytes
beyond the bounded path excerpt every diagnostic allows, except a keyword
name, which is at most 32 bytes of `0x21` through `0x7E`. An error the
image-path validation reports keeps that validation's operation, `CATALOG`,
as it would at collection.

## Determinism

The image depends on the manifest's decoded entries, the content bytes and
the options. The order of lines, the choice of relative or full form, the
use of `/set`, continuation and comments change nothing, because every form
yields the same records and the catalog sorts them. Reversing the lines of
a manifest yields byte-identical output, which is the harness's
reproducibility check. Generated content is the same on every host. The
manifest the extractor writes depends on the image, the selection and the
wall-offset interpretation, not on the host.

## Tests

Each passing case has a seeded failure that proves the check ran.

* `test/mtree.c` exercises the parser through the public call on
  in-memory manifests: each line form, `/set`, `/unset` and `/unset all`,
  continuation, comments and the signature, `\ooo` and `\\`, `..`, the
  root `.`, every carried keyword at its valid boundaries, each keyword of
  the host-object and unsupported classes, an unknown keyword, and for each
  accepted construct the one-byte change that refuses it: a `\s`, a `#`
  inside a word, a NUL, a CR, a 65th word, a line one byte over the limit,
  `mode=10000`, `time=1.0000000000`, a second line for the same path, a
  full entry without its directory, `link` on a file, `contents` on a
  directory, `size` off by one, holes that touch, overlap, end past `size`
  or appear with `zero`, a seed with `host`. The reported status, offset
  and path are asserted, not only the status.
* `test/manifest.c` accepts a wrong generated digest at input and expects
  planning to refuse it with the parser's record, for each generator, and
  refuses a file larger than the image, claiming a digest, by planning
  without a byte of it being hashed.
* Content tests compare `read_at()` of seeded and positional sources at
  random offsets and lengths with a reference generator written in the test
  from the definitions above, check that holes read as zeros and that
  `next_range()` tiles the file, decode positional blocks back to identity
  and offset, and check the SHA-256 implementation against the FIPS 180-4
  vectors; each with a corrupted expectation that must fail.
* Content-path tests build a tree with a symbolic link component, a link at
  the end, a directory, a FIFO, a `..` path, an absolute path and, where
  the host allows, a nested mount, and expect each refusal; a file replaced
  between collection and emission fails as changed, a file with a wrong
  digest fails during emission with nothing published, and `-n` with the
  same manifest succeeds.
* Round-trip tests create FAT, exFAT, QNX6 and QNX4 images from manifests,
  read them back, extract with `--mtree`, create again from the written
  manifest and the extracted tree, and compare image bytes under `-R`; a
  changed byte in the tree or a changed keyword in the manifest must change
  the result. A manifest produced by `mtree -c` and one by `bsdtar
  --format=mtree` from the same host tree must produce the same image as
  `-p` host import of that tree, which checks the parser against two
  independent writers of the format. The manifest `x --mtree` writes is
  read by bsdtar on every host and by `mtree(8)` on FreeBSD and Linux;
  macOS `mtree(8)` is recorded as not applicable.
* CLI tests cover the `@` operand with and without `-C`, `./@name`, two
  manifests sharing a directory and colliding on a file, a manifest merged
  with a host tree, `fds` accounting, `--mtree` publication, an existing
  name, failure leaving no file, `-k` entries listed, and `--mtree` with
  `-O` as a syntax error.
* The harness's creation cases emit manifests with seeded content and names
  as bytes, so a case with case-equivalent siblings or an NFC/NFD pair runs
  on every host; the [spec-testing design](design-spec-testing.md) owns
  those changes.
* `fuzz/mtree.c` as above. Sanitizer runs follow the build documentation.

## Decisions

* **An operand, not an option.** `@file` gives a manifest an operand's rank
  and `-C` with no further rule; a `--manifest=FILE` option would need its own
  rank and content-directory rules.
* **Host digests during the copy,** as [Digests](#digests) explains: each file
  is read once and `-n` reads no payload.
* **Generated digests after planning,** as [Digests](#digests) explains: the
  planner refuses a size the image cannot hold before any byte is hashed,
  so the bytes hashed are bounded by the planned volume rather than by a
  new budget, and `-n` still reports a mismatch.
* **BSD flags.** FreeBSD's `uarch`, `uhidden`, `usystem` and `urdonly` would
  map onto FAT attributes; `flags` other than `none` is refused until a user
  needs the mapping.
* **`uname` and `gname`** are accepted beside numeric ids, so bsdtar's default
  output imports; fsimg consults no user database.
* **Root metadata.** The root `.` line's fields are dropped like a host
  root's, consistent with host import, although QNX6 and QNX4 could store
  them; root metadata is revisited as a whole.
* **SHA-256** is FreeBSD's `sha256c.c`, imported with its notice, as
  [Digests](#digests) states.
* **No manifest from `t`.** A listing has no digests; `x --mtree` is the
  manifest contract.
