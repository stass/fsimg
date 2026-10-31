# Building and validating fsimg

Development requires Autoconf, Automake, Libtool, make, a C11 compiler, and a
C++ compiler. On macOS install missing tools with `brew install autoconf
automake libtool pkgconf ruby`. CLI integration and installation qualification
use Ruby 3.2 or later and pkg-config as development tools. On FreeBSD install
Ruby, `devel/rubygem-minitest` and `devel/pkgconf`. Python is optional, used
only to invoke the exempt research ImageFS decoder under `tools/qnx/`. The
FreeBSD CI job installs `lang/python3`, and the Linux check container installs
`python3`, to keep this optional comparison exercised.
Independent FAT tests additionally use `brew install mtools
dosfstools cmake`. Linux exFAT interoperability uses `exfatprogs`.
Partition-table oracles use `sgdisk` (`brew install gptfdisk`) and `sfdisk`
(`brew install util-linux`, found in its keg's `sbin` with its `fdisk`), with
mtools formatting and listing the partitions, and `hdiutil` on macOS. They
read the disks `sgdisk` and `sfdisk` write and verify the disks fsimg writes:
`sgdisk -v` on a GPT around every filesystem and on a disk composed by the
`layers-compose` check program, `sfdisk --sector-size 4096 --verify` on
4096-byte-sector tables (`sgdisk` has no sector-size setting for a file),
`sfdisk -d`, `fdisk -l` and `mdir` on MBR disks, and `hdiutil attach` of a
raw GPT disk. `test/layers-integration.sh` reports each missing tool and
skips only when none is present.
FreeBSD uses native `newfs_msdos`/`fsck_msdosfs`; install
missing development tools through ports, including `filesystems/mtools`.
The additional fatcat oracle is built from a pinned upstream checkout.
`configure` finds each tool on `PATH`; a tool given to it in a variable such
as `MKFS_FAT` or `FATCAT` must be an absolute path, as the specification tests
treat any other value as an unavailable tool.
Container oracles use `hdiutil` on macOS for DMG, `qemu-img` (`brew install
qemu`) for fixed VHD and as a reader of DMG, which it cannot write, and
`img2simg` and `simg2img` (`brew install simg2img`) for Android sparse
images. `test/containers-integration.sh` checks that fsimg reads what each
writes exactly as the raw disk the tools expand it to, reports each missing
tool and skips only when none is present. `img2simg` aborts on a disk that
is not a whole number of 4 KiB blocks, so its fixture sets the GPT disk
length. The same script checks the containers fsimg writes: `hdiutil
verify`, `attach -nomount` and `detach` (no privilege is needed for an
image file) and `convert -format UDTO`, `qemu-img info` and `convert` of
the DMGs and VHDs, `simg2img`, and `sgdisk -v` on the disk inside, each
expansion compared byte for byte with the raw disk; a DMG with a flipped
data fork byte must fail both `hdiutil verify` and `fsimg W -d`.
`qemu-img check` has no checks for fixed VHDs, and the script says so.
Compression oracles use `gzip` and `pigz` (`brew install pigz`):
`test/compression-integration.sh` compresses a raw image with each, as one
member, as two members `gzip` wrote separately and concatenated, and as
`pigz` blocks with and without independent dictionaries, and checks that
fsimg lists each as the raw image from the file and from standard input,
reports gzip and verifies it. It also checks what fsimg writes: `gzip -t`
accepts its output at levels 1 and 9 and as a stream, `gzip -dc` (zcat)
expands each to the image fsimg writes without `-z`, and the DMG inside a
`.dmg.gz` passes `hdiutil verify` on macOS. It reports each missing tool
and skips only when neither `gzip` nor `pigz` is present. configure records `gzip` as `GZIP_PROGRAM`,
because gzip itself reads `GZIP` from the environment as options.
Release archives include generated Autotools support and build without those
generators. The installed library's one third-party runtime dependency is
the system zlib, which DMG chunks (and gzip) require: `configure` finds it
through pkg-config, or failing that `zlib.h` and `-lz`, accepts
`ZLIB_CFLAGS` and `ZLIB_LIBS` to override both, and fails naming zlib when
none links. zlib is never vendored, so the host's security fixes apply to
the inflate that parses untrusted images. On macOS and FreeBSD it is part of
the base system; on Linux install the distribution's zlib development
package (`zlib1g-dev`, `zlib-devel`). `libfsimg.pc` lists it in
`Libs.private` for static linking.

The CLI requires native spawn-time closure of all unspecified descriptors:
`POSIX_SPAWN_CLOEXEC_DEFAULT` or `posix_spawn_file_actions_addclosefrom_np`.
Configure reports an error if neither interface is available. Host OS and libc
versions must supply one of these mechanisms; a resource-limit-bounded close
loop cannot prove descriptor isolation. `check-install` also tests configure's
refusal with both capability probes forced unavailable.

From a checkout:

```sh
autoreconf -fi
mkdir build
cd build
../configure
make
make check
make check-install
make check-security
make check-integration
make check-spec
make check-asan
make check-ubsan
make check-lsan
make check-msan
make check-scale
make check-cli-rss
make check-extended
make check-fuzz
make check-fuzz-extended
```

The tree builds warning-free under `-Werror` with Clang and with GCC. CI builds
and checks with Clang on Linux and macOS, and with GCC 13 and GCC 16 on Linux.
On macOS the Homebrew `gcc` formula installs versioned drivers; configure a
separate build directory with `../configure CC=gcc-<major> CXX=g++-<major>`.

`check` builds and runs native tests, C11/C++ header examples, checked library
examples, CLI integration with an independent JSON decoder, and an ASCII
source/build check. Test logs are in `test-suite.log` and individual `.log`
files. Fixtures are relative to the disposable build directory and cleaned on
success. CLI presentation regressions can also be run with `make test-cli fsimg`,
`./test-cli`, and `ruby ../test/cli/run.rb ./fsimg` from a separate build directory
(use `ruby test/cli/run.rb ./fsimg` for an in-tree build). The CLI suite uses
Minitest (`devel/rubygem-minitest` on FreeBSD) with normal Ruby invocation and needs no Bundler or specification-harness
helpers. It covers contextual help, exact stream
destinations, quiet success, bounded loss examples, layout rejection reasons,
terminal controls and final verification timing, alongside JSON and output
failure checks. Failed-creation regressions use a sparse 256 MiB source to
exercise FAT12 capacity rejection without copying payloads or creating an image;
they check plain errors, absence of trailing metadata notices, explicit cluster
qualifications and preserved JSON error fields.
`check-security` runs the publication and sandbox harnesses directly;
a sandbox skip is printed explicitly. Automake records exit 77 as SKIP.
The sandbox harness forks disposable single-threaded children, closes inherited
handles, disables core dumps, installs the profile, and attempts real forbidden
operations. It must run on the kernel being qualified. The Linux-only
`test-sandbox-setup` target renames its own setup syscalls to deterministic mocks;
it checks unavailable initial probes, old ABIs, and every later setup failure.
The kernel harness also denies file/directory deletion in output and confirms
scratch deletion remains available. Unexpected or partially applied setup
errors remain `FSIMG_EIO` and never
permit an automatic retry without restriction.

The [specification-driven tests](../specs/design-spec-testing.md) are Ruby
under `test/spec/`. They need Ruby 3.2 or later and the gems pinned in
`test/spec/Gemfile.lock` (Minitest, PropCheck and BinData), installed once with
`bundle install --gemfile=test/spec/Gemfile` from the source tree. The install
compiles Minitest's prism dependency where the Ruby does not ship the locked
version, as Ruby 3.2 never does. A `BUNDLE_PATH` set for the install, to keep
the gems outside the system gem directory, must also be set when the tests run.
On macOS use `brew install ruby` and put `$(brew --prefix ruby)/bin` first in
`PATH`, because the system Ruby is older; Ubuntu 24.04 installs `ruby ruby-dev
ruby-bundler`, and FreeBSD the `devel/rubygem-bundler` port. Configure records
the interpreter as `RUBY`; pass `RUBY=` to configure when the interpreter has
another name, such as FreeBSD's versioned `ruby33`.

The lock records Bundler 2.4.20, the 2.4 series that Ruby 3.2 and Ubuntu 24.04
ship, so every supported Bundler reads it. RubyGems loads Bundler 2.4.20 where it
is installed in the default gem path, and the interpreter's own Bundler
otherwise. `Bundler.setup` in `run.rb` sets `RUBYOPT`, `RUBYLIB` and
`BUNDLER_VERSION` in its environment, so every Ruby process the harness starts
loads the same bundle. `test/spec.sh` exits 77 (SKIP) when Ruby 3.2 is
absent, and `run.rb` when Bundler or a locked gem is; a bundle that fails to
load for another reason, such as a `Gemfile` changed without relocking, fails
the run. `run.rb` loads the bundle with Bundler's frozen and no-auto-install
settings, which outrank any Bundler configuration, so a test run never rewrites
the lock, installs a gem or reaches the network.

`make check` runs the quick profile and `make check-spec` runs it alone; `make
check-spec-extended`, part of `check-extended`, runs the nightly profile, and
`make check-spec-deep` the deep one. `./test/spec.sh PROFILE` runs a profile
directly, and Minitest's `SEED` environment variable fixes the test order a
summary reports. PropCheck draws are not seeded. Each run locks
`test/spec-results/<profile>/` under the build directory, empties it, and
writes `coverage.json` and `summary.txt` there; `make clean` removes it. A run
passes only when Minitest reports no failure, error or interruption, at least
one test ran, the case phase did not fail as a whole and no case failed or
ended as a harness error. A failing case leaves a record under
`test/spec-results/<profile>/failures/<id>/`; `sh PATH/replay.sh`, or
`./test/spec.sh --replay=PATH/case.json [PROFILE]`, rebuilds and reruns that
one case under the current rules and reports any drift from its recorded
classification. Its exit status suits `git bisect run`: 0 when fsimg passes
the case, 3 when it fails it, 125 when this revision cannot judge it, and 128,
which stops the bisection, for a usage error or a harness fault.
`test/spec/bin/report PATH/coverage.json` prints a run's per-rule coverage.

`replay.sh` runs from the build directory. To bisect a recorded failure from
an in-tree build, copy its `case.json` elsewhere first, since any run of the
profile empties its results directory, and rebuild fsimg at every step, since
`test/spec.sh` only runs the binary it finds. A revision without `bin/replay`
cannot judge a case, so the script skips it:

```sh
recipe=$(mktemp) && cp test/spec-results/quick/failures/ID/case.json "$recipe"
git bisect run sh -c 'test -f test/spec/bin/replay || exit 125
    make -j32 fsimg test/spec.sh || exit 125
    ./test/spec.sh --replay="$1" quick' sh "$recipe"
```

A stage that fsimg cannot be launched for on the host counts as a failed case,
so bisect on an idle host.

The preflight runs inside the results directory, so a build directory deep
enough that a case's longest path would pass the host's `PATH_MAX` fails the
case phase, as does any other preflight refusal, and the report names the
check that refused. CI installs the gems on macOS, Ubuntu and FreeBSD and fails
when the suite skips there. The GCC 13 container builds Ruby 3.2.11 to exercise
the 3.2 floor; the other GCC container installs the distribution's Ruby. Both
run the CLI suite and skip the specification tests when their locked gems
are absent.

The tests' base images are cached in `test/spec-cache/` under the build
directory, keyed by the base recipe and the versions of the tools that build
it; `make clean` removes the cache. Measured on macOS (Intel, APFS) with
mkfs.fat 4.2, mtools 4.0.49 and fatcat, a first run builds the 1440 KiB FAT12
formatter base in 0.12 s, the 16 MiB FAT16 base in 0.23 s, the populated
64 MiB FAT32 base in 0.77 s and a FAT16 builder base mtools populates with
the same tree in 0.36 s, each including the oracles' read-back: the
read-only checker, `mdir` and `mtype` of every file, and fatcat. A cached
base is read back again in 0.05 to 0.24 s, and copying one into a case
directory takes 1 to 9 ms. Population pipes each file's bytes to `mcopy -`,
so a build writes nothing to the cache but the image and its manifest. mkfs.fat writes the FAT16 base's zeros, so that base occupies
16 MiB on disk, while the sparse FAT32 base occupies about 1 MiB. The
independent builder writes a 130000-cluster FAT32 base holding the standard
tree in 0.2 s.

Sanitizer targets run the full suite in `sanitizer-<mode>/` beneath the current
build directory, which may be the source tree configured in place or a separate
build. Each run starts from scratch: `make distdir` copies the distribution into
a read-only `sanitizer-<mode>/src`, and `sanitizer-<mode>/build` configures that
copy with the current build's configure arguments (`config.status --config`),
followed by the sanitizer compiler, flags and `--disable-shared`. Nothing outside
`sanitizer-<mode>/` is written, development oracles given to configure such as
`FATCAT` carry over, and a file missing from the distribution fails the run as it
would fail `make distcheck`. `make distdir` runs `build-aux/check-dist.sh`, which
fails when a git-tracked file other than a `.gitignore` or an `fsspecs/` draft is
missing from the distribution, so a test file left out of `EXTRA_DIST` cannot
silently drop out of a sanitizer run; stage a new file before the run so the
check sees it. A release archive has no git metadata, and there the check passes. Compiler and sanitizer diagnostics name files in the
copy; edit the originals. `CC` and `CXX` in the environment select the sanitizer
compiler (default `clang`); `make -n` does not run the targets. `make clean`
removes the sanitizer builds and the fuzz harnesses. `check-asan` combines ASan
and UBSan, enables stack use-after-return and scope checks, and enables leak and
initialization-order checks on Linux. macOS ASan does not support leak or initialization-order
checks. ASan/fuzz runs halt on UBSan findings and print a stack trace. Standalone
LSan/MSan targets print SKIP on non-Linux hosts; this is not a passing check.
Linux MSan requires Clang and an appropriate runtime; an unsupported compiler
or runtime fails configuration rather than being reported as a pass. TSan is
not applicable to the serial library and process-only test harnesses.

MSan uses a separately instrumented zlib so it can track the bytes deflate
and inflate produce. `MSAN_ZLIB` names its installation prefix; set it at
configure time or as `make check-msan MSAN_ZLIB=/path/to/zlib-msan`.
The MSan target refuses a missing prefix. Ordinary builds and the other
sanitizers continue to use the system zlib. The Linux check image and CI
build their MSan dependency from Ubuntu's source package, outside this
checkout, with the same Clang toolchain as the test build. For a local
Linux setup, unpack the host's zlib source package and run:

```sh
sh build-aux/build-msan-zlib.sh /path/to/zlib-source /path/to/zlib-msan
```

The helper copies the source into a disposable build directory, compiles
the static library with MSan and origin tracking, and installs it under the
given prefix. It needs Clang and make; Ubuntu source-package extraction
also needs `dpkg-dev` and enabled `deb-src` repositories. No zlib source or
binary is vendored into fsimg.

Fresh worker and output-relay processes use fixed sanitizer settings, including
`abort_on_error=0`, `exitcode=86`, and `symbolize=0`. The supervisor classifies
that reserved status as a runtime failure after reaping both children, so a
negative protocol test cannot hide a sanitizer report as an expected I/O error.
A deliberate test-only ASan use-after-free in a fake child checks this actual
runtime path. Its expected report is suppressed with child stderr; no production
code contains that fault. Tracer-based leak scanning cannot run under these
profiles, so fresh children disable it and print an explicit instrumentation
subcheck skip. Parent and library leak scanning remains enabled where supported.
A successful worker test does not claim child leak-scan coverage.

Allocation failure injection is private to each context. Tests fail object,
slice, and transfer-buffer allocation without changing production allocators
or global process state. Callback scripts exercise interruption, short I/O,
EOF, partial errors, over-reporting, and flush errors. Publication hooks inject
file-sync, linking, cleanup, and directory-sync failures, including a lost
publication reply. The race harness runs competing exclusive destination
creation and checks which inode and bytes survived.

Metadata tests exercise resident and spilled records, cross-page fixed records,
bitsets, aggregate logical quotas, one-page dirty eviction, sparse zero ranges,
exclusive scratch creation, frame corruption, allocation failure, and scratch
open/resize/read/write failures. External sorting is checked against an
independent libc `qsort` oracle using explicit key/ordinal pairs, including
duplicate keys, empty and variable-length records, and varying cache/run/fan-in
settings. Failure checks verify that the sealed input remains readable and that
temporary memory, scratch, and descriptors return to baseline. Cancellation and
work limits are checked during run generation, merging, page I/O, and comparison.
The metadata interfaces are private; their borrowed session outlives them.

Source/catalog tests cover unordered entries, tuple-prefix ordering, deep and
wide paths, explicit parents, collisions, metadata framing, snapshot lifetimes,
and injected allocation failures during collection/preparation. Unicode tests
round-trip every scalar value and reject malformed UTF-8/UTF-16. Disposable source
trees exercise descriptor-relative reopening, changed files and directories,
symlink pivots, special files, hardlinks, scratch overlap, interrupted directory reads,
and descriptor/depth/cancellation limits. Provider scripts cover copied tables,
reentry, empty files, range progress and EOF handling, short reads, partial
errors, interruption, and cumulative work limits. Corrupt catalog, index, child,
and host-source records are rejected before values reach a provider or a copy
operation.

`test-fat-write` covers public construction/planning/emission, all FAT variants,
every supported sector/allocation-unit pair, exact and automatic sizing, full
volumes, variant thresholds, fixed-root exhaustion, maximum file-size planning,
case/alias conflicts, timestamp range and precision, reproducible epochs,
source-order/cache-size byte equality, successful slices, poisoned reused
sinks, FD/hardlink/slice aliases, source changes, allocation failures, short
zero-write work accounting, sparse publication, and retained-staging errors.
The integration script also validates fsimg-created images with `fsck.fat` and
extracts ordinary, nested, and Unicode content with mtools for byte comparison.

`test-metadata-loss` covers the writer's one metadata-loss operation: an
accepted mask counted once per field in the plan and the context with one
callback under the rendered path, refusals of required fields and of any field
without `FSIMG_WRITE_ALLOW_LOSS` that count and report nothing, absent or
unknown fields rejected as internal state errors, omissions and FAT encoding
losses sharing one total, a late driver refusal keeping earlier observations,
a capability-summary refusal reporting nothing, the QNX4 owner pair as one
loss, FAT's date-only access time, the shared whole-second selector's
endpoints, fractions, time bases and reproducible epochs, and DOS calendar
boundaries and leap days, each against values written out by hand.

`test-emit` exercises the common emission lifecycle through both the FAT12 and
exFAT drivers. Empty, zero-only, and mixed data/zero provider sources must give
identical images on clean sinks, stale sinks with and without a zero-range
capability, and sinks that accept short writes, and zero ranges are never read.
It injects source changes on empty, zero-only, and partly copied files, early
EOF, write failures at the first, middle, and last write with confirmed
progress, cancellation at the first, middle, and last poll, flush failure, each
allocation failure, and plan records whose payload exceeds its allocation or
the volume. Every failure must leave the output incomplete, no source open,
no memory, scratch, or descriptors charged, and the writer refusing a second
emission. A test emit callback also drives the copy operation directly with
placements no driver produces: several regions in non-ascending image order,
zero-capacity regions, region boundaries inside a batch and inside ZERO and
DATA ranges, an initialized length ending inside DATA, a planned length below
the catalog's, a region outside the volume, and sink and source failures in
mid-copy, checked against an independently mapped expected image.

`test-driver-contracts` and `test-layer-contracts` are the
[common contract suites](../specs/design-driver-contract-tests.md). Each first
reconciles its test profiles with the registry in both directions: every
advertised filesystem variant or layer has exactly one coverage profile whose
declared capabilities, kind and carried filesystem equal the advertised ones,
and every profile names a registered entry. The registry then schedules the
scenario families each profile owes, and the profiles' handwritten facts
supply the expected answers. Filesystem profiles (FAT12, FAT16, FAT32, exFAT,
QNX6 in both byte orders, QNX4, ImageFS) cover writer lifecycle, source
consistency, defined output on clean, poisoned, zero-range-less and
short-transfer sinks, reproducibility across contexts, cache budgets and the
published-file sink, sink failures and preflight refusals, cancellation,
entry/work/output/scratch limits and aggregate memory accounting, and
per-format metadata cases. Readers get lifecycle, a canaried bounded volume,
content delivery, transport faults, cancellation, limits, verification counts
and extraction. Allocation and scratch-operation faults are swept over every
position each phase reaches. A sweep proves its fault fired, and its first
unreached position is the final successful run. Layer profiles cover each mode's
wrap reproducibility, decoded bytes, family report, transport faults, sink
failures and allocation sweeps, and spooled layers (gzip, hbcifs) a spool
budget refusal. Seams cover a scheme inside a container, gzip
around a container, exact-volume refusals, a final transcode failure and the
carried-filesystem rule. Both programs print their case counts, and conditional
cases they skip are counted as not applicable (`CONTRACT_VERBOSE=1` lists
them). A failure names its profile, scenario, phase and fault position, and a
replay selector:

```sh
./test-driver-contracts exfat/writer-scratch
./test-driver-contracts 'qnx6-be/*'
./test-layer-contracts dmg-zlib/write
```

Reader fixtures are written by the common writer, so they are round-trip
integration evidence. Independent format evidence remains with the
format-local tests, the specification harness and the integration scripts.

`test-zlib-memory` covers the shared zlib allocator in
`lib/core/zlib-memory.c`. It checks charged deltas and zero-byte requests,
a budget refusal keeping its exact limit diagnostic, a sticky first error
across later attempts and frees, the `FSIMG_ENOMEM` fallback, every
allocation ordinal of deflate and inflate initialization returning to the
baseline, inflate's delayed window allocation, two streams sharing one
budget, poisoned allocation contents, reset and reuse, allocation sweeps
through the DMG and gzip encoders, and those encoders under a stepped-down
memory budget.

Targeted initialization and staging regressions are:

```sh
./test-fat-write --zero-work
./test-fat-write --short-zero-work
./test-fat-write --sparse-publication
./test-fat-write --retained-staging
./test-fat-write --corrupt-plan
./test-fat-write --scale
```

The 30,000-name writer planning case uses one shared cache page and a 16 MiB
memory budget. On APFS it reports 2,236,096 peak charged memory bytes,
17,730,160 peak logical scratch bytes, eight descriptors and 1,017,736 I/O calls.
This measures planning independently of payload transfer and logical capacity.

FAT tests use an independently encoded sparse fixture family across all 96
combinations of three variants, four sector sizes, and eight cluster sizes.
They exercise type thresholds, legal maximum cluster identifiers, FAT12 window
boundaries, fragmented sequential/random reads, long-name sequences and alias
collisions, directory sharing/cycles, global ownership, mirrored/active FATs,
FSInfo/backup headers, bounded slices, corruption, source changes, allocation
failures, cancellation, and scratch faults. A maximum FAT file has 4 GiB minus
one logical byte; its sparse test transfers only boundary markers and verifies
allocation without reading through four gigabytes. Logical FAT32 geometry
ceiling checks do not claim a physical large-image traversal.

`check-integration` creates images with the host's formatter: dosfstools on
macOS/Linux or native `newfs_msdos` on FreeBSD. Native `fsck_msdosfs` or
`fsck.fat` checks them; mtools supplies population and Unicode extraction, and
fatcat supplies additional ASCII, nested long-name and fragmented reads on
macOS and FreeBSD. Linux runs the formatter/checker and mtools checks, with
only fatcat subchecks explicitly skipped. The ordinary suite includes this
script and skips it if a required development tool is absent. The dedicated
target returns that skip rather than claiming success. The manifest is
`test/fat-integration.sh.log`: formatter
and validator versions, geometry, population commands, a forced fragmentation
recipe, and outcomes. Fixtures include all FAT variants and a 4096-byte-sector,
8192-byte-cluster FAT16 case. Bytes and Unicode names are checked, then complete
payload verification runs. No test mounts an image or needs root.

`check-fuzz` builds and checks a separate `sanitizer-fuzz` static library with
ASan/UBSan and `-fsanitize=fuzzer-no-link` coverage on every object, then
compiles `fuzz/fat.c` with Clang's libFuzzer runtime, and creates seeds for all
FAT variants using the independent fixture builder. When Ruby 3.2 and the
harness's gems are installed, `test/spec.sh --export-seeds` adds the
specification harness's seeds, one per targeted or promoted FAT case that does
not force a format with `-t`, rewritten on every run; seeds of cases that no
longer exist are removed. A separate exFAT corpus combines an independent
raw-table fixture with a writer-generated compressed-table image. It runs 1000
mutations per format corpus with a 3 MiB input ceiling and a ten-second
per-input timeout. Each reader has a 16 MiB budget, 1000-entry, eight-level and
250,000-work limits, and no scratch authority. Each input is read both as an
exact-length byte array and as a prefix of zero-filled logical storage. The
latter reaches FAT32's minimum geometry without a large resident seed; it does
not allocate or transfer the logical volume. The target fuzzes probe,
directory/name parsing, chain traversal, content/range reads, index
construction, and full verification; it also checks cleanup accounting.
`fuzz/qnx6.c` forces the QNX6 decoder and reads each input as the exact slice,
since QNX6 finds its second header from the slice length; its seed is a 64 KiB B
= 512 volume from the independent generator holding every entry kind, inline and
long names and a depth-1 tree, with a 1 MiB input ceiling and the same budgets.
`fuzz/qnx4.c` forces the QNX4 decoder and reads each input as the exact slice,
since QNX4 derives its block count from the slice length, and again cut to whole
blocks, which a length-changing mutation otherwise never reaches. It requires
the verify request to fail as unsupported without work. Its seeds are the reader
tests' sample volume, 2000 blocks holding every entry kind, 16-, 48- and
505-byte names, a hard link and a 61-extent file, and a 400-block volume whose
file has 122 extents in a three-block chain, with a 1 MiB input ceiling and the
same budgets. `fuzz/ifs.c` forces the ImageFS decoder and reads each input
as the exact slice and again cut to the `image_size` its header declares,
since bytes past the image are never read and a length-changing mutation
otherwise never reaches the record table. It lists every entry, reads the
first and last bytes of each file and symbolic link, and requires every
opened image to fit the slice and every resource to be released. Its seed,
from `test-ifs --seed`, is a little-endian image with every record kind,
implicit parents and names outside the profile, with a 1 MiB input ceiling
and the same budgets. `fuzz/codec.c` drives the LZO1X and NRV2B decoders directly:
the input's first four bytes choose the codec, the limit or capacity and the
LZO1X call plan, and the rest is the stream. Each LZO1X stream is decoded in
one call and again in pieces through a small window keeping 49151 bytes of
history, and the two must give the same verdict at the same offset and the
same bytes; every call must progress or stop at an edge and leave its
unreported output poisoned. Its seeds, from `test-codec --seed`, are every
stream the unit tests check, valid and refused, and, when
`FSIMG_CODEC_FUZZ_CHUNKS` names the directory `test/codec-integration.sh`
filled, native boot-image chunks; the input ceiling is 65540 bytes. An
input whose first byte has bit 2 set is instead a block for the LZO1X
encoder: it is encoded by an encoder kept across inputs and by a fresh
one, which must give the same bytes within a buffer of exactly the bound,
the stream must decode to the block, and a capacity one byte below the
bound must be refused with nothing written; `test-codec --seed` writes
such seeds as `lzoenc-*.seed`. The same target builds `fuzz/protocol.c` and runs 1000 mutations
with a 64 KiB input ceiling and five-second timeout. It exercises production
framing, job codecs, and typed INFO/PLAN/ENTRY/VERIFY validators. Worker
lifecycle ordering is covered by `test-worker`, not inferred from pure wire
fuzzing. `fuzz/gzip.c` forces gzip on each input and opens it as a file
and, up to 16 KiB, as a pipe stream, each into a fresh unlinked spool under
a 4 MiB limit, so the member header parser, inflate, the trailer checks,
the trailing-byte rule and the spool reservation see every mutation; its
seed is a two-member file with FEXTRA, FNAME, FCOMMENT and FHCRC that
`test-compression --seed-gzip` writes, with a 256 KiB input ceiling.
`fuzz/mtree.c` feeds each input to the mtree(5) manifest parser
twice, whole and in 13-byte reads, under a 256-byte path limit, depth eight
and a 100,000-line work limit; both parses must reach the same status after
the same entries, every accepted entry must satisfy the parser's contract
(valid image path, link, contents path, holes within size, metadata), and
the parser must return all its memory. The same bytes are then written to
a file and imported through `fsimg_writer_add_manifest()`, without a content
directory and with a small fixed one, under a 512-unit work limit; an
accepted manifest's catalog is prepared and its digest claims on generated
content are checked as planning checks them, which hashes each claimed file
whole with no image to bound its size, and its generated and zero sources
must tile under `next_range()` and
read back, zero inside every ZERO run, and each import must return all its
memory and descriptors. Its seeds, written by `test-mtree
--seed`, are `mtree -c` and bsdtar manifests of one host tree and a
generated-content manifest, with a 64 KiB input ceiling.
`fuzz/container.c` opens each input under detection and under each forced
container, `ifsboot` and `hbcifs` among them, each with a fresh unlinked
spool under a 16 MiB limit, so their header checks and LZO1X and NRV2B
decodes see every mutation; its seeds are the images `test-containers
--seed-dmg`, `--seed-vhd` and `--seed-simg`, `test-ifsboot --seed-raw`,
`--seed-lzo` and `--seed-ucl`, and `test-hbcifs --seed-hbcifs` write, with
a 1 MiB input ceiling. Set `FUZZ_CC` to a Clang with the required runtime; `FUZZ_CXX` defaults to its
sibling `clang++`. Apple Clang lacks the libFuzzer runtime; macOS fuzz
validation uses Homebrew LLVM:

```sh
brew install llvm
FUZZ_CC="$(brew --prefix llvm)/bin/clang" make check-fuzz
```

The exact standalone commands from the top of a separate build directory are:

```sh
make test-fat test-exfat test-qnx6 test-qnx4 test-ifs test-codec \
    test-protocol test-layers test-containers test-compression test-mtree
MAKE=make sh ../build-aux/fuzz.sh
```

Set `MAKE` to the make program that drives the build (`gmake` on FreeBSD); in a
tree configured in place the script is `build-aux/fuzz.sh`. It builds the
instrumented library through `build-aux/sanitize.sh fuzz` and compiles the
harnesses into `sanitizer-fuzz/` from the same read-only source copy.

The target's `fuzz-corpus/`, `exfat-fuzz-corpus/`, `qnx6-fuzz-corpus/`,
`qnx4-fuzz-corpus/`, `ifs-fuzz-corpus/`, `codec-fuzz-corpus/`,
`protocol-fuzz-corpus/`, `image-fuzz-corpus/`, `container-fuzz-corpus/`,
`plist-fuzz-corpus/`, `gzip-fuzz-corpus/`, `mtree-fuzz-corpus/` and libFuzzer
crash artifacts are disposable build outputs. Existing corpus seeds and
mutations are reused; `make clean` keeps the
corpora, `make distclean` removes them, and neither removes crash artifacts.
`FUZZ_SECONDS=60 make check-fuzz` selects a finite wall-clock allowance per
corpus instead of the 1000-mutation smoke budget. The value is decimal
seconds from zero through 999999. `check-fuzz-extended` uses 600 seconds per
corpus. A smoke run is not a sustained campaign.

`check-scale` is a separate, non-sanitized qualification. It sorts 24 MiB
of generated records and fills/queries a 24 MiB fixed-record array with a 16 MiB
context budget, a 16-page shared cache, a 256 KiB run allowance, and merge fan-in
eight. It prints charged memory, logical and sampled physical scratch peaks,
I/O counts/bytes, elapsed time, and the host's raw `getrusage` maximum RSS value
(bytes on macOS, KiB on Linux/FreeBSD). Generated data and fixed-record lookups
have deterministic expected results without a resident scale oracle. It also
prepares a 100,000-file root directory at a 16 MiB budget and a grouped
100,000-file catalog at a 256 MiB budget. The generated provider supplies
deterministic content without host payload fixtures. Catalog tests check every
path, source reference, and parent, sample provider bytes, and report peak charged
memory, scratch, and descriptors, cumulative I/O calls, and elapsed time.
Scratch should use a disk-backed filesystem; a RAM-backed directory consumes
system memory outside the library heap budget.

The FAT scale case lists and structurally verifies 65,520 empty files in one
directory at a 16 MiB context budget and one shared cache page. It requires
scratch use above that budget, checks on-disk order and ownership, and reports
charged memory, scratch, descriptors, I/O, wall time, and process RSS:

```sh
./test-fat --scale
```

On APFS this case reports 2,190,928 peak charged memory bytes, 28,632,432
logical scratch bytes, 28,639,232 sampled physical scratch bytes, five owned
descriptors, and 2,134,938 I/O calls in 104.821 seconds. Process `ru_maxrss` is
5,382,144 bytes. These measurements cover enumeration and structural verification
of empty files, not payload throughput or the CLI/worker RSS gate.

The QNX6 scale case grows one dimension of a generated sparse volume at a
time: `N` to 67,108,864 blocks (a depth-2 bitmap tree), the inode capacity to
1,048,576, one directory to 200,000 files, a file's tree to depth 3, and a
file past 4 GiB. Each volume is listed with every file read and verified
with payload, each at a 16 MiB context budget:

```sh
./test-qnx6 --scale
```

It checks the driver design's resource bounds as they apply to these
volumes: every live inode listed and verified, charged memory at most 4 MiB
at every size, scratch within a context budget linear in `N`, the inode
count, the live inodes and the blocks objects own, no owned block read twice
by one operation, and the system run read in bounded passes, so that only
verification reads the bitmap and the inode table in full, once each. The
per-inode reads and bitmap-window loads allowed beside those passes are
regression ceilings for the reader's access pattern, not design bounds. It
prints image I/O calls and bytes, the bytes read from the system run and
from content blocks, all I/O calls including scratch, peak memory and
scratch, and time. On APFS charged memory peaks between 3,235,840 and
3,303,104 bytes in every case and operation. The 200,000-file
directory peaks at 21,821,982 scratch bytes and is listed in 6.1 seconds and
verified in 10.1; the 67,108,864-block volume verifies in 0.55 seconds with
8,388,624 scratch bytes, 8,388,608 of them its block bitset, and the 4 GiB
file in 0.82 seconds. Ordinary `test-qnx6` runs the same cases at small sizes.

The QNX4 scale case grows one dimension of a sparse volume generated by
`test-qnx4`'s own generator at a time: `N` to the format's 2,147,483,648
blocks, a terabyte with a 256 MiB bitmap; one directory to 200,000 direct
names; one file to 65,535 one-block extents, the most the record's 16-bit
count holds, in a 1,093-block extent chain; and a tree to 16,383 nested
directories with a file at the bottom. Each volume is listed with every
file read and verified with payload, each in a fresh 16 MiB context and a
child process of its own, which reads the volume's file with `pread`:

```sh
./test-qnx4 --scale
```

It checks the driver design's resource bounds as they apply to these
volumes: every name listed and every file, directory and allocated block
verified; charged memory at most 4 MiB and scratch within a budget the
context enforces (the claimed bitset of `N` bits, 128 bytes per name and 32
per extent), which together keep the 256 MiB claimed bitset out of memory;
the operation's growth of the child's peak RSS at most 32 MiB; every
extent block read exactly once by each operation; directory reads at most
each directory's slots once, the root's twice, and each name's record once
more; the bitmap read in full only by verification, once, beside one
window load per 4 KiB window holding owned blocks and two per extent block;
and the `.inodes` table read in full only by verification. On APFS:

| Case | Operation | Image calls | Image bytes | Bitmap bytes | Directory bytes | Extent-block bytes | Peak scratch | RSS | Seconds |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `N` = 2^31 | list | 15 | 38,594 | 12,800 | 16,512 | 0 | 268,435,472 | 5,120,000 | 0.002 |
| `N` = 2^31 | verify | 65,553 | 268,482,242 | 268,448,256 | 16,512 | 0 | 268,443,724 | 5,144,576 | 1.627 |
| 200,000 names | list | 206,261 | 25,633,973 | 4,608 | 25,616,960 | 0 | 21,008,436 | 7,364,608 | 5.721 |
| 200,000 names | verify | 206,264 | 25,646,193 | 8,636 | 25,616,960 | 0 | 21,008,436 | 7,368,704 | 5.673 |
| 65,535 extents | list | 79,838 | 39,753,244 | 5,614,080 | 16,448 | 559,616 | 1,594,174 | 5,926,912 | 0.122 |
| 65,535 extents | verify | 68,835 | 39,778,469 | 5,631,113 | 16,448 | 559,616 | 1,594,174 | 7,041,024 | 0.112 |
| 16,383 levels | list | 32,777 | 9,465,412 | 3,072 | 9,453,056 | 0 | 0 | 15,069,184 | 1.317 |
| 16,383 levels | verify | 32,780 | 9,476,164 | 5,632 | 9,453,056 | 0 | 0 | 11,816,960 | 1.314 |

The per-kind byte counts exclude the format registry's probe of the image's
first 8,704 bytes, which the image bytes include, and give the metadata
passes. At `N` = 2^31 the listing loads the bitmap windows of the
self-check and of the walks, and verification adds one pass over all 268,435,456 bitmap bytes, compared a
window at a time with the claimed bitset, which lives in scratch: the
verification's 65,553 image calls and 131,115 I/O calls in all are one
4 KiB window of each at a time, while the process stays at 5 MB. The wide
directory is read twice, 12.8 MB in its slot pass and 64 bytes per name
again when each entry is replayed, one image call per name. Every
operation reads each of the 1,093 extent blocks once, and the extents
case loads about 1,370 bitmap windows, one and a quarter per extent block,
since a block's own bit and the extents it describes lie in different windows.
Verification reads the 8,192-byte `.inodes` table once beside open's
signature read, and the listing never. Charged memory peaks between
3,236,264 and 3,818,032 bytes, the deep tree's walk stack of 32 bytes per
level of `max_depth` included; the 200,000-name directory's scratch peak is
its sorted key records, about 105 bytes per name. The RSS column is the
child's peak, almost all of it the operation's growth. Ordinary
`test-qnx4` runs the same cases in memory at 100,000 blocks, 2,000 names,
2,000 extents and 255 levels, the file at the default 256-component depth
limit; it exits 77 when the host cannot hold the 64 GiB sparse volume of
the bitmap-window case.

The QNX4 writer's scale cases write no image file. Each emits into a
callback sink that keeps only the volume's nonzero 4 KiB pages in memory,
sharing identical ones, and reads the volume back through the same sink:
the common emitter zeroes the sink through `zero_range` first and skips
zero sources, so free blocks and zero payload are never written. The sink
refuses more pages than the plan's metadata and the tree's content need,
so a writer that wrote free space fails the case instead of filling
memory. Every case runs in a child process with an explicit scratch budget
and a scratch directory the parent creates and removes, under
`QNX4_SCALE_SCRATCH` (such as a RAM disk) or the current directory, and
only when that filesystem has the scratch budget and 1 GiB more free; a
skipped case makes the run exit 77:

```sh
./test-qnx4-write --scale
QNX4_SCALE_SCRATCH=/Volumes/scratch ./test-qnx4-write --scale-limits
```

`--scale`, part of `check-scale`, runs under a 16 MiB context budget. It
emits a full 2^31-block volume, 1 TiB: the tree, 511 files of 2^31 - 1
zero bytes and a filler that takes every remaining block, so the bitmap is
ones throughout and the last extent ends in block `N`. It reads the tree's
files back with their checksums, verifies the structure, and requires
verification to find no free block. It also emits one directory of
200,000 files, one in sixteen with a 49-byte name, whose catalog and rows
spill past the budget into scratch. Both check charged memory at most
8 MiB, the child's RSS growth beyond the sink's pages at most 64 MiB, and
the bytes written at most the plan's metadata and the tree's content. On
APFS:

| Case | Memory peak | Plan scratch | Scratch peak | Emission I/O calls | Zeroing calls | Bytes written | Distinct pages | Nonzero pages | RSS growth | Plan s | Emit s | Read s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2^31 blocks, full | 5,491,216 | 0 | 268,630,479 | 4,698 | 16,777,216 | 268,480,766 | 25 | 65,558 | 19,250,072 | 0.01 | 1.39 | 11.55 |
| 200,000 files | 5,491,816 | 125,438,790 | 125,438,790 | 239,686 | 3,432 | 20,056,704 | 4,890 | 4,900 | 21,539,376 | 5.03 | 1.84 | 3.80 |

The full volume's scratch peak is the reader's claimed bitset of 2^31 bits;
the writer plans its 538 entries in memory. Emission writes the 256 MiB
bitmap, ones that share one page, and its metadata in 4,698 calls; zeroing
the 1 TiB sink first takes 16,777,216 calls of 64 KiB that touch nothing.
The wide directory's scratch is its catalog and rows, about 630 bytes per
entry. The whole run takes about 24 seconds with the free disk space
unchanged.

`--scale-limits` plans only, at the format's 2 GiB object limit, under a
1 GiB context budget: 4,194,288 long names fill `.longfilenames` and one
more is refused with ".longfilenames exceeds 2 GiB", and a directory of
33,554,366 children fills its 2 GiB of directory blocks and one more child
is refused with "directory exceeds 2 GiB" at `/d`. Each case's metadata
cache holds 272 MB, a quarter of the budget; the rest of the catalog and
its sort runs spill. With scratch on a RAM disk:

| Case | Status | Volume bytes | Memory peak | Scratch peak |
| --- | ---: | ---: | ---: | ---: |
| 4,194,288 long names | OK | 2,685,015,040 | 271,960,632 | 3,019,890,212 |
| 4,194,289 long names | ERANGE | | 271,960,632 | 3,019,890,932 |
| 33,554,366 children | OK | 2,148,025,856 | 271,960,632 | 20,535,325,945 |
| 33,554,367 children | ERANGE | | 271,960,632 | 20,535,326,557 |

The long-name cases plan in about a minute each and the directory cases in
five to nine minutes each, so the run needs about 21 GiB of free scratch
space and a quarter of an hour.

The ImageFS scale case grows one dimension of an image `test-ifs`
synthesizes on demand from its description, so neither the test process
nor the disk ever holds it: the body to 2^32 - 4 bytes, holding a file
whose content is the 4 GiB of zeros between the table and the data and the
first four bytes of the data, and a four-byte file ending at the trailer;
the record table to a million file
records in a thousand implicit directories; the root to a million files;
65,536 records whose paths are all at the 1024-byte limit, five components
of 204 bytes; and ten files at depth 512, the most a 1024-byte path holds,
below 511 implicit directories. Each image is listed, every file's name
and bytes checked, and verified with payload, each in a fresh 16 MiB
context and a child process of its own:

```sh
MallocSpaceEfficient=1 ./test-ifs --scale
```

It checks the driver design's resource bounds for these images: every
member and implicit directory listed and verified; charged memory at most
8 MiB, the reader's fixed state and a metadata cache of at most a quarter
of the budget; scratch within a budget the context enforces, four times
the table and 64 bytes per entry; the operation's growth of the child's
peak RSS at most 32 MiB, below the million-record table; the million-record
and million-file cases spilling past the 16 MiB budget; open reading the
table once, where a table read cut a record re-reading it, less than one
record per 4 KiB of table; a listing reading each file's record once more
at replay, two bytes and then the record, and each file's bytes once; and
verification adding exactly one pass over the image for the checksum.
macOS's default allocator keeps freed blocks resident, so that the
million-record listing's RSS growth varies from 18 to 73 MB between runs
at the same 5.4 MB charged peak; `MallocSpaceEfficient=1`, which
`make check-scale` sets, makes it return them, and on macOS the RSS bound
is checked only under it, the run printing a skip otherwise. Ordinary
`test-ifs` checks no RSS bound: its cases are small, and a sanitizer's
quarantine keeps freed memory resident. On APFS:

| Case | Operation | Image calls | Image bytes | Table bytes | Table passes | Data bytes | Memory peak | Scratch peak | RSS growth | Seconds |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2^32 - 4 body | list | 65,545 | 4,294,976,084 | 212 | 1.77 | 4,294,967,076 | 3,239,249 | 0 | 4,673,536 | 0.203 |
| 2^32 - 4 body | verify | 131,081 | 8,589,943,376 | 332 | 2.77 | 8,589,934,152 | 3,304,801 | 0 | 4,562,944 | 1.529 |
| 1,000,000 records | list | 3,005,380 | 94,051,852 | 90,043,056 | 2.05 | 4,000,000 | 5,356,513 | 105,105,496 | 6,959,104 | 10.876 |
| 1,000,000 records | verify | 3,006,113 | 142,051,980 | 134,043,088 | 3.05 | 8,000,000 | 5,422,065 | 105,105,496 | 6,975,488 | 11.619 |
| 1,000,000 root files | list | 3,005,380 | 94,051,852 | 90,043,056 | 2.05 | 4,000,000 | 5,356,929 | 160,000,512 | 6,955,008 | 10.435 |
| 1,000,000 root files | verify | 3,006,113 | 142,051,980 | 134,043,088 | 3.05 | 8,000,000 | 5,422,481 | 160,000,512 | 7,192,576 | 10.526 |
| 65,536 1024-byte paths | list | 205,974 | 146,565,800 | 146,294,860 | 2.11 | 262,144 | 5,356,929 | 204,670,147 | 7,163,904 | 5.332 |
| 65,536 1024-byte paths | verify | 207,039 | 216,296,232 | 215,763,052 | 3.11 | 524,288 | 5,422,481 | 204,670,147 | 6,991,872 | 4.674 |
| depth 512 | list | 35 | 30,780 | 21,944 | 2.07 | 40 | 3,249,745 | 0 | 4,550,656 | 0.121 |
| depth 512 | verify | 36 | 41,508 | 32,536 | 3.07 | 80 | 3,315,297 | 0 | 4,640,768 | 0.124 |

The per-region byte counts exclude the format registry's probe of the
image's prefix, which the image bytes include. Table passes are the table
bytes read over the table's length: one pass at open, one more as replay
reads each file's record again, which with its two-byte length read is a
little more than a pass, and verification's checksum pass; the tiny body
and deep tables have few records, so the probe's share and the replay of
records longer than their share of the table move the ratio. The million
records take 44,000,032 table bytes; their path and child streams, sorted
copies and group table peak at 105 MB of scratch, 2.4 times the table,
while the one directory of a million files adds common directory
preparation's sorted keys and peaks at 160 MB, 3.6 times. Charged memory
peaks at 5.4 MB, of which about 3.2 MB is the reader's fixed state and the
rest the metadata cache before stores spill, and RSS grows by at most
7.2 MB. The 4 GiB body costs the listing one 64 KiB read per 64 KiB of the
spanning file and verification twice that, the checksum and the payload.
The whole run takes about 56 seconds on a loaded host. Ordinary
`test-ifs` runs the same cases at a 1 MiB body, 20,000 records, 20,000
files in the root, 200 paths at the limit and depth 64. The RSS bound has
been measured on macOS alone ([TODO](todo-ifs.md)).

The extended ten-million-file catalog uses the same generated provider, 10,000
explicit directories, a 256 MiB budget, and no retained per-file object or FD:

```sh
./test-catalog --scale 10000000
```

`check` also runs the comparison baseline, `test/baseline.sh`. It detects
semantic and resource changes in library internals on seven named fixtures and
finishes in under two seconds. `fat12-tree`, `fat16-tree`, `fat32-tree` and
`exfat-tree` write one shared tree reproducibly from provider sources into a
memory sink, then list the image and read all content back. The tree has
names that need generated FAT aliases, a 300-entry directory, a 24-level path,
an empty file, a zero-only file and a mixed data/zero file. Each context has
a 16 MiB budget and one metadata cache page, so the writer's catalog spills to
scratch. `exfat-zero-tail` reads a hand-built file whose initialized length is
shorter than its logical length and whose tail cluster is poisoned.
`fat-fragmented` and `exfat-fragmented` run one fixed access script on
hand-built fragmented files: a sequential pass, short reads at fixed offsets,
then range queries interleaved with reads. `./test-fat-write --baseline`,
`./test-exfat --baseline` and `./test-fat --baseline` print the records:
one `fixture=<name> key=value ...` line per fixture. The script sorts them and
compares them with the checked-in `test/baseline.txt`.

Keys ending in `_sha256` (image bytes, traversal lines, content read) and
`image_bytes` must match exactly. Every other key is a resource counter:
I/O calls, bytes read, memory, scratch and descriptor peaks, metadata work,
and `*_memory_retained`. That last key is the charged memory at the end of
a phase, before teardown, so it shows any index, plan or cache kept
resident. A counter fails when it exceeds `ceil(baseline * 1.10)`. Decreases and growth within
that tolerance are printed but do not fail. A missing or extra fixture or key
fails. The script classifies keys in one place. To cross-check image digests
with `shasum -a 256`, set `FSIMG_BASELINE_DUMP` to an existing directory.
Each created image is then also written there as `<fixture>.img`. Images are
created exclusively, so use an empty directory for each run.

`make baseline-record` regenerates `test/baseline.txt`; the test itself never
writes it. Re-record only for an intended change, and explain every changed
value in the commit message. The baseline is recorded on macOS. Linux and
FreeBSD must pass within the same tolerance. A deviation on another host is
investigated, not re-recorded away. Sanitizer builds compare the same
counters, which do not depend on instrumentation.

Apart from the comparison baseline, the ordinary suite has twenty-six tests.
macOS reports twenty-three passes and three skips (backend unavailable,
Linux-only setup mocks, and unavailable exfatprogs). Linux reports
twenty-six passes; FAT fatcat subchecks explicitly skip by host policy
while dosfstools/mtools coverage runs. The user-provided
FreeBSD 16/ZFS host reports twenty-four passes and two skips (Linux-only setup
mocks and unavailable exfatprogs), including native FAT/mtools/fatcat
interoperability. See [platform evidence](platform-evidence.md)
for guest kernels and filesystems. The metadata scale fixture reports a
329,088-byte allocation peak, 75,795,888-byte logical scratch peak, and 134,683
I/O calls on APFS. These are fixture measurements rather than general bounds.

The instrumented fuzz smoke run uses Homebrew LLVM 23.1.2 on macOS. FAT
completes 1000 mutations from its FAT12/16/32 seeds and the 80 specification
harness seeds (`cov: 2406`, `ft: 6844`), exFAT completes 1000 mutations
(`cov: 2291`, `ft: 6359`), QNX6 completes 1000 mutations (`cov: 2524`,
`ft: 7590`), and protocol completes 1000 mutations (`cov: 409`, `ft: 770`).
None reports an unintended sanitizer or accounting finding. These counters
describe finite runs and vary with the generated mutation sequence.

The 100,000-file wide catalog reports a 3,157,912-byte allocation peak,
58,500,368 scratch bytes, seven descriptors, and 86,607 I/O calls. The grouped
case reports 5,255,064 allocation bytes, 61,558,724 scratch bytes, seven
descriptors, and 75,894 I/O calls. These charged allocation figures exclude
kernel filesystem caches and scratch storage.

The extended run validates 10,000,000 files beneath 10,000 directories in
754.155 seconds. It reports 5,255,064 peak charged memory bytes, 6,155,866,048
logical scratch bytes, seven descriptors, and 10,976,692 I/O calls at a
256 MiB budget. This is catalog/source qualification; it does not measure a
filesystem image, payload transfer, or the CLI/worker RSS acceptance gate.

`check-extended` performs actual exFAT planning for ten million generated files
and for a separate 100,000-entry wide directory, reads/verifies/extracts a
million-fragment file, compares fixed-content volumes differing by 1000 times,
and runs `check-cli-rss`. These are disk-backed, non-sanitized qualifications;
the ten-million-file plan needs about 6.2 GB of scratch. `check-cli-rss` runs all
six image commands on 100,001 real source files at a 16 MiB library budget and
checks their combined memory allowance. Its FreeBSD negative guard rejects
pre-existing mappings above the inherited virtual-memory cap; that
platform-specific subcheck explicitly skips elsewhere. See the
[system qualification evidence](qualification.md) for commands, exact
measurements, memory methods and limitations.

Stage reviews include the required independent code-review agent and an external
read-only review. Write a prompt covering correctness, design, security and
information leaks, and the style guide's readability review, mark new files
with `git add -N`, and run `aireview "${TMPDIR:-/tmp}/review.md" prompt.txt`
from the checkout. `aireview`, installed from `~/aireview` and documented in
its README, reviews the checkout and `git diff HEAD` read-only, its reads
confined to the checkout and the system's minimal paths, with Codex
`gpt-6.1-sol` at reasoning effort `xhigh`. It
switches to OpenCode `zai-coding-plan/glm-5.3 --variant max`, with a warning,
only when Codex is out of quota, rate limited or refused by its content
filter, and the first line of the
review names the reviewer used. The diff holds every staged and unstaged
change to a tracked file, so keep unrelated or sensitive edits out of the
tree. Review substantive fixes again, and report a failed or unavailable
review explicitly. The reviewers are development tools and introduce no
library runtime dependency.

The FAT writer has a completed `zai-coding-plan/glm-5.3 --variant max` review
and focused follow-ups of alias-cursor structure, cleanup ownership, and
plan/source length guards. No Critical/High or readability findings remain.
The suspected FAT32 ceiling issue was checked against the Microsoft reference:
MAX is CountOfClusters + 1, and allocatable identifiers are below the bad-cluster
marker; the implemented boundary follows that rule. Preflight host-source
reopening deliberately validates identities before any output mutation, in
addition to validation around payload copying.

Installation and release checks:

```sh
make check-install
make install DESTDIR="$PWD/install-root"
make uninstall DESTDIR="$PWD/install-root"
make distcheck
```

`check-install` makes a clean distribution snapshot, configures a separate
private prefix with static and shared libraries, and checks install/uninstall,
pkg-config metadata, C/C++ consumers, relocated CLI operation and native runtime
dependencies. It uses its own layout even if the caller's build is in-tree or
uses custom installation directories. Compiler choices and flags are preserved;
package lookup and build configuration overrides cannot redirect the fixtures.
Failure logs and fixtures are retained at the printed location. The metadata
self-test rejects foreign, relative and extra dependency flags and accepts
absolute prefixes containing spaces. The command embeds its library core;
moving the executable does not require a private library or helper search path.

Release-archive qualification runs `./configure` and the complete ordinary suite
with installed Autotools generator names, including versioned automake/aclocal,
shadowed by commands that fail if invoked. The generated `.tar.xz` archive
passes this check and `make distcheck`; source checkouts still require
`autoreconf -fi`.

`include/fsimg.h` documents implemented entry points and ownership. An options
or callback table must be initialized and have exactly the supported size and
version. The ABI is experimental. Library statistics include allocation headers
and the context itself, and exclude caller buffers, allocator bookkeeping,
thread stacks, mappings, and kernel caches. Cumulative I/O statistics saturate
at `UINT64_MAX`. Memory and descriptor quotas aggregate across live handles;
private session counters persist across incremental calls. An output ceiling
of zero is derived by the containing reader/writer from its volume length; optional
work ceilings are disabled at zero.

The installed header/library expose FAT/exFAT readers, volume information, current-file
content/range reads, full verification, and independent reader/writer capability
enumeration. The installed `fsimg` executable provides all seven commands in
[fsimg(1)](fsimg.1). Public writers expose
collection, immutable planning, raw emission, and durable no-replace publication.
Source access and common catalog/storage remain private. Public extraction exposes safe private-tree creation and optional times.
Image staging and publication share a library coordinator. Executable modules
implement worker supervision and output relay; `test-worker` provides a
same-executable frontend for fault checks. `test-staged-verify` is a test-only
copy of the command, built with its `main` renamed, whose creation worker
corrupts the staged image after it completes, so a whole `cW` meets a failed
verification without a fault hook in the shipped `fsimg`. [libfsimg(3)](libfsimg.3) and the
[checked examples](examples.md) explain embedding and failure handling.

Extraction and worker qualification:

```sh
make test-extract test-worker test-staged-verify test-protocol test-sandbox \
    test-sandbox-setup
./test-extract
./test-worker
./test-staged-verify
./test-protocol
./test-publish --identity
```

`--identity` is a separate qualification probe, outside the passing ordinary
suite. It can fail on FreeBSD/UFS when ctime changes after publication; retain
the conservative source-change error and consult the open investigation.

Extraction tests cover complete and selected trees, case/alias resolution with
canonical displayed paths, pruning unrelated corrupt directories, quota and
allocation failures, existing destinations and symlink pivots, metadata loss,
and postorder timestamp restoration. Protocol/worker tests cover split and
malformed frames, bounded chunked paths, forged field values and publication
claims, READY/warning/ACK ordering, context/FD startup failures, inherited FD
closure above lowered limits, raw data before entry validation, interrupted
request writes, deadlines, worker death, broken/blocked consumers, relay
confirmation, and incomplete extraction. The extractor counts logical length
before creating holes; format-specific initialized-data tails receive their
own driver regressions.

The separate actual-kernel mount fixture checks that a confined scan crosses
a mount point beneath a source root. It needs root only to mount a
disposable alias. Run the test itself as an ordinary user, and always
unmount before removing the fixture:

```sh
fixture=$(mktemp -d)
mkdir "$fixture/source" "$fixture/lower" "$fixture/source/alias"
printf sentinel > "$fixture/lower/payload"
# Linux; FreeBSD uses: mount -t nullfs "$fixture/lower" "$fixture/source/alias"
sudo mount --bind "$fixture/lower" "$fixture/source/alias"
./test-worker --mount-source "$fixture/source"
test "$(cat "$fixture/lower/payload")" = sentinel
sudo umount "$fixture/source/alias"
rm -rf "$fixture"
```

Ordinary image use and the normal test suite require no mounts or privileges.
Source-change regressions force an observable timestamp difference so coarse
host timestamp granularity cannot accidentally leave the mutation invisible.
The library detects metadata-visible changes; hostile mutation requires a
stable snapshot as specified by the source contract.

Sanitizer execution evidence:

| Host | Command | Result |
| --- | --- | --- |
| macOS | `make check-asan` | 49 pass, 6 tool/platform skips; the specification suite passes |
| macOS | `make check-ubsan` | 49 pass, 6 tool/platform skips; the specification suite passes |
| macOS | `make check-fuzz` | 42 pass, 5 tool/platform skips; the FAT, exFAT, QNX6, QNX4, protocol, image, container, plist, gzip and mtree fuzzers complete 1000 runs each |
| macOS | `make check-lsan check-msan` | Unsupported-host SKIP for each, not passing coverage |
| Linux (container) | `make check` and `make check-asan` | 53 pass, 2 QNX integration skips each |
| Linux (container) | `make check-lsan` | 53 pass, 2 QNX integration skips |
| Linux (container) | `make check-msan` | 53 pass, 2 QNX integration skips, with the external instrumented zlib |
| FreeBSD 16/ZFS | `make check` | 49 pass, 6 tool/platform skips; the specification suite passes |
| FreeBSD 16/ZFS | `make check-asan` | 39 pass, 7 skips; test-format's fixture fault is fixed and passes in the same build |

Native disk-input checks provision a disposable read-only GPT disk, compare
extracted payload bytes, and exercise info/list/verify/extract without a
payload spool. They never select an existing device. Build `fsimg` and
`test-device` first, then run from the build directory:

```sh
sh /path/to/fsimg-source/build-aux/device-check.sh "$PWD/fsimg" 512
sh /path/to/fsimg-source/build-aux/device-check.sh "$PWD/fsimg" 4096
```

macOS uses `hdiutil` without mounting, tests block/raw and partition endpoints,
and explicitly skips the 4096-byte fixture and confinement check. Linux uses
read-only loop devices and a private read-only node; FreeBSD uses read-only
md/GEOM providers and tests partition endpoints. Linux/FreeBSD provisioning
needs noninteractive sudo or root; parsing runs as the invoking user with
required confinement. The native child verifies that prepared geometry can be
adopted and read after ioctl and ambient-open authority are removed. A Linux
container must receive the test loop-device facility explicitly; the ordinary
Linux check container supplies none. Unit tests substitute the host boundary
of the production device adapter, including non-power-of-two sectors, and do
not require a device or privileges.

Device-input qualification records the following full-suite results. Every
configuration below has zero failures; skips remain excluded from passing
coverage.

| Host | Configuration | Full-suite result | Native device fixture |
| --- | --- | --- | --- |
| macOS | Ordinary; ASan/UBSan | 60 pass, 5 skips each | 512-byte block, raw and partition endpoints; confinement and the 4096-byte fixture are unavailable |
| Linux in Docker | Ordinary; ASan/UBSan; standalone LSan; MSan with instrumented zlib | 63 pass, 2 skips each | Read-only 512/4096-byte loop endpoints with required confinement |
| FreeBSD 16 on p620 | Ordinary; ASan/UBSan | 59 pass, 6 skips each | Read-only 512/4096-byte md and partition providers with required confinement |

The native fixtures compare payload bytes, extract beneath a checked empty
directory and verify ioctl denial on confined hosts. The private-directory
fixtures and the FreeBSD sanitizer suite also pass with umask `002`.
The device adapter's deterministic tests cover confirmed read prefixes followed
by error, EOF or cancellation, including logical error offsets, untouched
buffer tails and work/I/O accounting. Repeated fixed-VHD selection, slicing
and reopening preserve partition byte ranges and decoded sector provenance.
The finite sanitizer fuzz smoke suite passes with Homebrew LLVM and the
default 1000-run allowance per harness:

```sh
FUZZ_CC=/usr/local/opt/llvm/bin/clang \
    FUZZ_CXX=/usr/local/opt/llvm/bin/clang++ make -j32 check-fuzz
```

From a macOS development host, `build-aux/linux-check.sh [-r REV] [COMMAND]`
checks a committed revision, `HEAD` by default, on Linux. It builds an Ubuntu
24.04 image from `build-aux/linux-check/Dockerfile` (the CI Linux job's
packages with their recommendations, the runner tools the tests use as
oracles, a UTF-8 locale, and the revision's spec gems), feeds the revision in
as an archive, configures it out of tree with clang, and runs COMMAND, a
single shell string, in the build directory as an unprivileged user. COMMAND
defaults to `make -j8 check` plus the CI check that the specification suite
passed; sanitizer targets such as `make check-lsan check-msan` run the same
way. When it fails, the script prints the tail of each failed test's log.
Building the image uses the network, and runs the revision's Gemfile; the
check itself runs without network access. Uncommitted changes are not
checked, and nothing built on Linux lands in the working tree. Each image
rebuild leaves the previous image behind; `docker image prune` reclaims them.
Docker caches the base image and the package layer indefinitely, while CI
installs current packages on every run. When a result differs from CI,
refresh the toolchain: `docker pull ubuntu:24.04` and
`docker image prune -a --filter label=fsimg-linux-check` make the next check
rebuild the image from current packages.
It needs Docker, for example through colima. For the Linux sandbox
profiles to run rather than skip, the Docker VM's kernel must provide
Landlock ABI 3 (Linux 6.2 or later), as Ubuntu 24.04's stock 6.8 kernel
does. For `make check-msan`, the VM's `vm.mmap_rnd_bits` must be at most 28,
as on GitHub's runners: with more address randomization, clang 18's MSan
runtime re-executes itself through `personality(ADDR_NO_RANDOMIZE)`, which
Docker's default seccomp profile denies, and configure fails with "cannot
run C compiled programs". Set it with
`colima ssh -- sudo sysctl -w vm.mmap_rnd_bits=28`; it lasts until the VM
restarts.

Configure searches the standard system sbin directories for formatter/checker
tools. Linux runs dosfstools/mtools and exfatprogs interoperability in its
ordinary and sanitizer suites. macOS and the provided FreeBSD host include
passing independent FAT interoperability. FreeBSD uses its native
formatter/checker plus mtools and fatcat. The real
child ASan fault is intentional. MSan reports the uninstrumented system
zlib's output in the compression tests, so its qualification is incomplete
until the msan profile links an instrumented zlib (TODO.md).

Native directory batches are initialized by the compatibility adapter, including
padding written through raw kernel interfaces without sanitizer interception.
The focused `./test-cli --private-cleanup` regression exercises this boundary
under MSan, symlink-safe scratch deletion, and retained-directory reporting.

Build the fatcat development oracle outside the source tree and give its path
to configure, exported or as a `FATCAT=` argument. Configure records development
tool paths given to it (see `configure --help`), so sanitizer and fuzz builds use
the same oracles; a tool found on `PATH` is searched for again there:

```sh
sh build-aux/build-fatcat.sh /tmp/fsimg-fatcat-oracle
export FATCAT=/tmp/fsimg-fatcat-oracle/build/fatcat
```

The destination must be new. The helper needs Git, CMake, a C++ compiler and
patch; it preserves the upstream checkout/license and verifies revision
`76000303082e9e86f1aca8edb030c5fb8a63ae55`. FreeBSD selects upstream's existing
native getopt/64-bit-offset interfaces with a two-condition patch. No format
logic is changed. C++14 supports the pinned source's deprecated APIs.
CI builds this oracle on macOS and FreeBSD. On FreeBSD, missing
tools are installed through ports; ordinary builds/tests remain unprivileged.
Fatcat's non-512-sector subcheck is explicitly skipped, and its Unicode name
decoding is not an oracle; native checks and mtools cover those features.

`./test-fat --volume-labels` covers native formatter labels with the archive
bit and malformed label placement/allocation/attributes. Automatic FAT sizing
keeps sixteen clusters away from the type cutovers as Microsoft recommends;
`./test-fat-write --type-margins` verifies both sides and retains exact legal
minimum counts. Native formatter fixtures specify both total sector count and
adequate FAT capacity explicitly rather than relying on host file-geometry
inference. The FSInfo next-free value remains an advisory search hint; the
native checker's warning does not trigger fixture repair.

Extraction/worker code, packaging, native-oracle integration, volume labels,
and automatic sizing margins have completed read-only OpenCode reviews with
`zai-coding-plan/glm-5.3 --variant max` on the Z.ai coding-plan subscription.
No Critical/High/Medium or readability finding remains in those paths. The
local reviewer checks the same criteria before pre-commit test runs.

The exFAT tests cover boot and entry-set checksums, malformed upcase encodings,
volume-specific case mappings, timestamp bases and losses, bitmap/FAT ownership,
fragmented metadata and content, preallocation, and initialized-data boundaries.
Poisoned backing bytes beyond ValidDataLength must become logical zeroes through
content reads, raw worker output, and host extraction. A 512-fragment fixture
checks unique bytes in sequential and random reads and bounds metadata work.
Allocation failure injection exercises construction and cleanup. Reproducibility
compares complete images across insertion orders, cache sizes and poisoned sinks.
Publication tests inject cancellation during a successful staged-file flush
and require cancellation to be observed before the no-replace link; post-link
failures retain their published/durability outcome.

`test/exfat-integration.sh` checks emitted images with exfatprogs and verifies
an independently formatted image. Linux uses exfatprogs 1.2.9; macOS and FreeBSD
explicitly skip this tool-specific test. An isolated Linux kernel also mounts
an emitted image read-only and compares all 70,003 zero-tail bytes. A separate
64 MiB image formatted by exfatprogs and populated through the kernel contains
Unicode names, nested directories and fragmented content; fsimg verifies six
files, two directories and 196,707 initialized payload bytes. These separate
mount checks require fixture setup privileges; ordinary tests do not mount.

`test-qnx6` builds classic QNX6 volumes with its own generator, written from
the format design with separate offset tables, a long-division CRC and its
own name checksum; it shares no code with `lib/formats/qnx6/`. It reads every
entry kind, inline, 28-byte and 510-byte names, UTF-8 names and full metadata
in both byte orders at all four block sizes with checksums on and off, then
verifies payload and free counts. Further cases cover depth-2 fragmented
files, inode trees of depth 0, 1 and 2, inode tables spanning several
top-level indirect blocks at depths 1 and 2, bitmap trees of depth 1 and 2
(the latter a sparse 4 GiB volume at B = 512), an object in block N - 1, a
full volume with no free block or inode at every block size in both byte
orders, a directory chain at the context's depth limit, short and padded
slices, two snapshots in either order, the mounted bit, and over eighty
single-field mutations of headers, trees, inodes, names, directories and the
bitmap, each checked for its expected status. Fault cases inject allocation
failures at every step; reads cut down to one byte; a source that stops
making progress, as at EOF, inside each header, the bitmap, an inode, the
root directory, file content and verification's inode-table pass, failing
at exactly that offset; slices truncated at and between every 4096-byte
boundary; an image file that shrinks after open; a failing read range;
scratch open, resize, read and write failures with every metadata store
spilled; cancellation at each of the first 64 polls and then a spread of
later ones through a listing, a verification and an extraction, with and
without spilled metadata; and a work limit. Each checks the reported error
and that no charged memory, scratch or descriptor outlives the reader. The
ordinary run also runs the scale cases above at small sizes. Sparse volumes
live in an unlinked temporary file in `$TMPDIR` (or `/tmp`), which needs a
64-bit address space and room for sparse files; a host without them prints
a `SKIP:` line for each such case and the program exits 77 after every
other case passes. `--seed` writes the fuzz seed,
`--read-external` lists and verifies an image file, and `--scale` runs the
scale cases at full size.

`test-qnx6-write` creates QNX6 volumes through the public API and checks each
one twice: with fsimg's reader and payload verification, and with its own
checker, which decodes the layout from the format design with separate
offsets, CRC and name checksum, requires every tree in the post-order run
layout, every block below `A` to have exactly one owner, the bitmap to match,
and every other byte of the image, from the boot area to the tail past the
last 4096-byte boundary, to be zero over a sink poisoned with `0xa5`. A
mutation pass shows the checker reporting one changed byte in a free block,
the twin region, the header, an unused slot and the bitmap. The fixture tree
holds every entry kind, inline, 27-, 28- and 510-byte and UTF-8 names and a
depth-1 file, and is written at all four block sizes in both byte orders;
forward and reverse catalog order, a one-page and a 1024-page metadata
cache, scratch on and off and two time zones give identical bytes. Further
cases cover exact sizes, sizing with inode capacity, reserve and free space,
every invalid setting, the `.boot` rules, the metadata and time policy, a
provider link target with a NUL byte, sink-size, durability, write, flush,
short-write, changed-source and early-EOF faults, and allocation, metadata
storage and cancellation failures at every step. `--scale` publishes a
100000-entry directory (100008 inodes, 4.3 MB peak memory, 60 MB peak
scratch), a 4000000-inode table, a depth-3 file of 130 MiB at B = 512, an
exact 8 GiB volume of 512-byte blocks and a 4 GiB + 4097-byte file at
B = 4096, each sparse on disk and passing the checker. `--native IMAGE`
compares fsimg's empty volume with a native empty capture of the same
geometry, and `--create DIR ORDER BLOCK BYTES [UUID]` writes the fixture tree
for the native qualification that the [QNX6 research
record](qnx6-research.md#fsimg-writer-output) documents.

`test/qnx6-integration.sh` reads external evidence that is never
redistributed, and skips what is absent; [the QNX6 infrastructure
page](qnx6-infrastructure.md) records where that evidence and the QNX 6.5.0
VM that produced it live:

```sh
FSIMG_QNX6_F1=/path/to/test_image.bin \
FSIMG_QNX6_NATIVE=/path/to/captures FSIMG_QNX6_LARGE=1 \
    ./test/qnx6-integration.sh
```

`FSIMG_QNX6_F1` names the NFI research fixture, checked against its pinned
SHA-256. `FSIMG_QNX6_NATIVE` names a directory of QNX 6.5.0 captures
(`*.img.gz`, and with `FSIMG_QNX6_LARGE=1` and zstd the 5 GB `*.img.zst`
ones); each must verify except the torn-header capture and the oversized
format that QNX itself refused, and each empty capture formatted with
long-name checksums must match fsimg's empty volume of its geometry. On macOS
all 31 native captures and F1 behave so: free block counts equal native
`chkqnx6fs` output, the eight empty comparisons find no difference outside
the CRC and sync time, and the 9,830,368-block volume with 1,048,576 inodes and a depth-3 file verifies with payload in under
a second at 16 MiB RSS. The four QNX6 partitions of the measured PCM 3.1 HDD,
read with `fsimg W --offset= --length=` at their partition offsets and
lengths, also
verify, including the head unit's original populated partition. On a Linux
host as root the script mounts the generated sample with the kernel's qnx6
driver and compares every regular file with `fsimg x -O`; elsewhere that
comparison is an explicit skip, and no Linux result is recorded
([TODO](todo-qnx6.md)).

`test-qnx4` builds QNX4 volumes with its own generator, written from the
format design with separate offset tables; it shares no code with
`lib/formats/qnx4/`; it includes the driver header to compare the
production decoders with its encoders and to read two facts no public
interface reports, `lost+found` occupancy and, in `--manifest`, stored link
counts. The generator lays volumes out as `dinit` does and fills free
blocks and unused file tails with `0xa5`. The sample volume holds every
entry kind, 16-, 48- and 505-byte names, a nested tree, a hard link in
`.inodes`, a 61-extent file and boundary owners, modes and times; it is read
at 1000, 4095, 4096, 4097, 5001, 8192, 8193 and 40000 blocks, so the bitmap
ends inside a byte, at a byte and at a block in one, two and three blocks,
and spans ten blocks, more than one 4 KiB bitmap window. Further
cases cover names of 1 to 505 bytes at each record's boundary, files of 0, 1,
2, 60, 61, 62, 121 and 122 extents, two- and sixteen-block directories, a
2000-entry directory spilled to scratch, a `.inodes` table of 15,600
records grown to 62 extents with a two-block chain of its own, a file at
the default 256-component depth limit and one past it, the minimum
43-block volume, a slice of `2^31 + 1` blocks refused by the geometry check
without a read past the 2000 blocks behind it, probe
exclusivity against fsimg's FAT, exFAT and QNX6 volumes and a native-like
QNX6 boot area, and about 200 single-field mutations of the geometry, root
block, bitmap, names, long-name records, statuses, records, link targets,
extent blocks, dot entries, `.inodes` and `lost+found`, each checked for its
status class, with every status byte value of each record kind. Fault cases
inject allocation failures at every step, scratch open, resize, read and
write failures with a one-page cache, cancellation at every poll, a work
limit, failing reads of the bitmap, an extent block and file content,
seven-byte short reads, a source that ends inside the root block, the
bitmap, the root directory, `.inodes`, `lost+found`, a directory, an extent
block and file content, also behind three-byte short reads, and inside
verification's `.inodes`, long-name and bitmap passes, each failing at the
exact offset, and an image file truncated after open, a changed source;
each checks that nothing charged outlives the reader. A 64 GiB sparse
volume of 32,769 bitmap blocks puts bitmap blocks past the 32,768 blocks
open's first window describes: a clear bit of block 32,768 fails the open,
of block 32,769 or of the last bitmap block only verification, as an owned
block marked free; a scratch budget below its `N / 8`-byte claimed set
fails the open, and a cancellation halfway through verification stops the
bitmap comparison. Every case runs full verification with payload too, which must
agree with the listing's class or, for the faults only verification sees,
fail with its own: allocated orphans, a file's data in an extent block or
in a metadata block, link counts of every record kind, an unnamed used
`.inodes` slot or long-name record, and the incomplete reports of an
invalid label, a used `lost+found` slot and an unsupported nested object.
The fault cases run under verification as well, with read failures of the
blocks only it reads: free `.inodes` slots, unused long-name records and a
bitmap block between the two windows open reads. Extraction and a hard
link's chain walked once per name are checked too. `--seed` and `--seed-chain` write the
fuzz seeds, and `--manifest IMAGE [OFFSET LENGTH]` prints one line per
object for the integration script.

`test-qnx4-write` creates QNX4 volumes with the writer. Every image is
checked twice: with fsimg's reader and payload verification, and with its own checker, written from
the format design with separate offsets and sharing no code with
`lib/formats/qnx4/`. The checker decodes the root block, the bitmap, every
directory slot, `.inodes`, `.longfilenames` and `lost+found`; requires the
writer profile's layout (the formatter's fixed slots and records, every
object one extent, allocation one prefix of blocks, siblings in byte order,
`.inodes` slots and long-name records assigned in a row) and the native
statuses, modes and link counts; and requires every byte the layout gives
no meaning, the loader block, free slots, slack and free blocks included,
to be zero over a sink poisoned with `0xa5`. A mutation pass shows it
reporting one changed byte in the loader, a free block, a free slot, an
unused `.inodes` slot, a file's slack, a dot entry, a bitmap padding bit
and root slot 6, and an empty root's slot 7 losing its `0x02` status. The
fixture tree holds every entry kind, names of 1, 16, 17, 48, 49 and 505
bytes, with 15, 47 and 504 in a separate case, two long names sharing their first 48 bytes, a
UTF-8 name, directories with direct, link and long names, `/.boot` with
content and nondefault owners, modes and times; forward and reverse catalog
order, a one-page and a 1024-page metadata cache, scratch in fresh
directories and none, and two time zones give identical bytes. Further
cases cover the 43-block empty volume, exact and automatic sizes at the
bitmap-block boundaries 4096/4097 and 8192/8193, requested free space, a
full volume, the 2^31-block and 2^31 - 1-byte limits, roots of 0 to 60
entries, `inodes=` below, at and above the need, the `/.boot` and
`/.altboot` rules, reserved root names, names outside the profile, the
owner and time loss policy, labels of 0, 1, 47 and 48 bytes, every invalid
setting, sink-size, durability, write, flush, short-write, changed-source
and early-EOF faults, allocation and metadata-storage failures at every
step and cancellation at polls about 6% apart, each followed by a resource
check.

`--native DIR` compares fsimg's empty volume with each empty `dinit -hq`
capture in DIR, the integration script's `FSIMG_QNX4_NATIVE`, at its block count,
label and format time; block 1, the proprietary loader, is not compared,
and free blocks the formatter never wrote, the capture disk's `0xaa` fill,
are counted rather than compared. On macOS all twelve captures match byte
for byte: the whole-disk 131072-block volume with and without `chkfsys`,
with the 9-byte label `LabelAAAA` and the 48-byte label of `labelC`, and
the 1000, 4095, 4096, 4097, 5001, 8192, 8193 and 131071-block volumes,
root slot 7's status `0x02` and slot 6's `lost+found` link included.
`--create DIR NAME BLOCKS [LABEL]` writes `DIR/writer.img` and its manifest
`DIR/writer.manifest`, in the integration script's line format, for the
native qualification in `tools/qnx/guest/qualify4.sh`: NAME `empty`,
`tree` or `full` (the tree and a zero-filled file leaving 256 blocks free),
BLOCKS 0 for the smallest volume. The images are not reproducible, so the
entries keep their own times:

```sh
./test-qnx4-write --create /tmp/q4/tree tree 131072 PCMnav
./test-qnx4-write --create /tmp/q4/empty empty 0
./test-qnx4-write --create /tmp/q4/small tree 0
./test-qnx4-write --create /tmp/q4/b1 tree 4096
./test-qnx4-write --create /tmp/q4/b2 tree 8192
./test-qnx4-write --create /tmp/q4/b3 tree 12288
./test-qnx4-write --create /tmp/q4/full full 131072
```

Compressed with `gzip -9` they take 66,688, 326, 1,552, 3,472, 5,524,
7,558 and 66,749 bytes, far below the serial loader's 3 MiB limit. Each runs
through `capture.sh -i DIR/writer.img NAME "sh /tmp/qualify4.sh BLOCKS"`,
with `ro` as a second argument for the volumes with no free block (empty
and small). A volume at the PCM navigation partition's 102767805 blocks,
52.6 GB sparse, is too large for the loader: it is written while the VM is
stopped to the file `FSIMG_QNX_HD1` names, which the VM then attaches as
its second disk, and `qualify4.sh hd1` checks and modifies that disk in
place, so it must be a disposable copy.

`test/qnx4-integration.sh` reads external evidence that is never
redistributed, and skips what is absent; [the QNX4 research
record](qnx4-research.md) describes the captures and the partition:

```sh
FSIMG_QNX4_NATIVE=/path/to/captures-qnx4 ./test/qnx4-integration.sh
FSIMG_QNX4_H1=/path/to/disk.img FSIMG_QNX4_H1_OFFSET=BYTES \
    FSIMG_QNX4_H1_LENGTH=BYTES ./test/qnx4-integration.sh
QNXPROBE='/path/to/qnxprobe' ./test/qnx4-integration.sh
```

The script first writes the generator's sample volume with `test-qnx4
--seed` and verifies it. `FSIMG_QNX4_NATIVE` names a directory of QNX 6.5.0
captures (`q4-*.img.gz`). Each must open as QNX4 through `fsimg i`, list
through `fsimg t` and verify completely through `fsimg W -d`; for
each `q4-manifest-*` capture the guest's `ls -l` and `cksum` manifest must
equal fsimg's names, kinds, modes, link counts, owners, sizes, symbolic-link
targets and the checksums of every file read through `fsimg x -O`, and
`test-qnx4-write --native` must find no difference. On macOS all 41
captures open and verify, the 16 manifests match and the twelve empty
captures equal fsimg's empty volumes; with the H1 partition below the whole
run takes 252 seconds, most of it one `fsimg x -O` per file.

With `FSIMG_QNX4_LINUX=1` on a Linux host as root, with the kernel's qnx4
driver, the script mounts
the sample and every capture read only from a loop device limited to the
volume's slice, and requires the kernel's view to agree with fsimg's
manifest and `fsimg x -O`: every entry below names of at most 48 bytes with
the same kind, size or symbolic-link target and POSIX `cksum`, and the same
number of entries in all, since Linux shows a name over 48 bytes as its
48-byte link field; it prints the kernel revision with each result. It also
mounts a writer tree and compares the `cksum` of every file the writer was
given with its manifest, counting the files below a name over 48 bytes.
`QNXPROBE` names a qnxprobe command at a pinned revision, split into words
so it may carry an interpreter, run as `$QNXPROBE IMAGE` on the exact slice
of the sample and of every capture; it must accept each volume fsimg
verifies. Both comparisons are skips on this macOS host, which reports
"the Linux qnx4 kernel reader needs FSIMG_QNX4_LINUX=1, a Linux host, root
and the qnx4 driver; this is Darwin as uid 501" and "QNXPROBE is not set",
so no Linux or
qnxprobe result is recorded ([TODO](todo-qnx4.md)).

`FSIMG_QNX4_H1` names a disk image read in place at the partition's byte
range. The script runs only `fsimg i`, which must report QNX4, and
`fsimg W -d`, whose payload read-through reads every file, and prints
only the verify record's counts. The PCM 3.1 HDD's QNX4 partition verifies
with 22 directories, the root included, and 52 other entries, 22,384,057,279
initialized bytes read, 43,744,183 blocks allocated and 59,023,622 free:
every allocated block has one owner. On macOS with the image in the page
cache, `fsimg W --offset=3232535040 --length=52617116160` takes 4.2 seconds
of
wall time and 0.22 seconds of CPU at a peak RSS of 41 MB as
`/usr/bin/time -l` reports it, and `W -d` 7.9 seconds at 37 MB.

`test-ifs` reads QNX ImageFS images built by its own generator, written
from [the format design](../specs/design-ifs.md) with separate offset
tables; it shares no code with `lib/formats/ifs/`. The generator lays out
the header, the mountpoint, one record per member in table order, the
terminator, the file data and the checksum trailer, in either byte order,
and the tests mutate the finished bytes. A second oracle, a model written
with a different algorithm from the driver's, inserts every record into an
in-memory tree in table order, a later record replacing an earlier one with
the same path and missing parents synthesized, and predicts the status of
open, every delivered entry and every `info` count, and the status and
report of full verification: every image that opens is verified
structurally and with payload reads and compared with it. The cases cover probe
exclusivity against fsimg's FAT12, exFAT, QNX6 and QNX4 volumes and a QNX4
root block at byte 512 that both formats claim; each header bound at `x -
1`, `x` and `x + 1`; the mountpoint's terminator, padding and profile;
record lengths 0, 1, 23 and 24, below each kind's minimum and crossing the
table, the terminator, the inode flag nibbles, extended attributes and
every field of the four kinds; implicit parents at depths 1, 32 and 512 and
the default depth limit; duplicate paths, records shadowed below a file, a
link and a FIFO in either table order and at several depths, and odd
paths; components of 1, 255, 256 and 1024 bytes and names outside the
profile, omitted with their subtrees and counted; content ranges at and across the trailer and the table, shared
ranges and zero-length files; every off-by-one of a symbolic link's target;
boundary owners, modes and times; boot and script references naming one
file, none, two records, only a shadowed record, a directory, the root, a
link and a FIFO; a flipped byte in the header, the mountpoint, its padding,
a record, record padding, file data and the trailer, a byte-swapped
trailer and a big-endian image summed in the wrong order; nonzero padding
under a correct trailer; a volume one byte and one sector past the image;
an invalid mountpoint; omitted and shadowed accounting; the checksum
pass's work charge per window; extraction; and
400 random trees in either byte order compared with the model.

The fault cases run a listing that reads every file and link, and full
verification with payload read-through, under every failure the library
can inject, and after each one require the context to hold no charged
memory, scratch or descriptor beyond its own and the scratch directory to
be empty. On an image whose 207 records span two 8 KiB table reads,
allocation fails at every occurrence and each kind of scratch operation
(open, resize, read and write, with a one-page metadata cache so every
store spills) at every occurrence up to the 64th and at growing steps after
it, until the operation needs no more; allocation failures end the
operation with `FSIMG_ENOMEM` and scratch failures with `FSIMG_EIO`
carrying the injected `ENOSPC`; reads of at most one and seven bytes change
nothing, the listing still matching the model. On the sample image,
cancellation at every poll and the work limit at every value end each
operation with `FSIMG_ECANCELLED` or `FSIMG_ELIMIT`. With the format
forced, so that the probe reads only the 88-byte header and every later
read is the driver's own, the n-th read of the image fails, and in a
second loop returns no bytes, for every n until the operation makes fewer
reads, which reaches every read site of open, replay, content and
verification; each ends the operation with `FSIMG_EIO` at that read's
offset, an end of the source reported as EOF. On the 207-record image a failed
read of the mountpoint, a record or the terminator fails open, of file
content the listing, and of the trailer only verification; and the source
ending inside the mountpoint, a record of the first and of a later table
read, the terminator, a record at replay, a link's record, file content
(also behind three-byte reads) and the trailer fails with `FSIMG_EIO` at
exactly that offset.

```sh
FSIMG_IFS_NATIVE=/path/to/captures-ifs ./test-ifs --native
```

`test/ifs-integration.sh` runs `test-ifs --native`, which reads external
evidence that is never redistributed. `FSIMG_IFS_NATIVE` names the native
captures directory described in [the ImageFS research
record](ifs-research.md); every file in its `cases/ifs-basic`,
`cases/ifs-names`, `cases/ifs-extra`, `cases/ifs-data`,
`cases/ifs-e10-sense` and `decoded` subdirectories that holds an ImageFS at
offset 0 is read and compared with the model: the status of open and, for
an image that opens, every entry's kind, metadata and bytes, every `info`
count, and the status and report of verification. Every image that opens
must verify, except in `cases/ifs-e10-sense`, the E10 mutations, where
every image but the unchanged one must fail. It prints only counts, and
the capture's file name on a mismatch, never a member name; without the
variable it prints `SKIP: FSIMG_IFS_NATIVE is not set` and exits 77. On
macOS the 84 images give 77 opened, 2 unsupported, 5 corrupt and none
mismatched, and 72 verified and 12 rejected, in 0.8 seconds: the eleven E10
mutations and the unsupported capture made to hold odd paths are the
rejected ones, and every decoded S1 and S2 body verifies. The other
unsupported image is the E10 copy with a flipped mode. The capture holding
a file record's descendant and the six decoded H3 boot
bodies holding two symbolic-link records below a symbolic-link record open
with those records shadowed. Images `dumpifs -u` decompresses are not compared:
no such copies exist on this host.

`test-ifsboot` reads QNX boot images through the `ifsboot` container
with the public API alone. A generator builds each image in memory: an
opaque boot prefix (none, or the 1,088-byte x86 BIOS loader), a startup
section of random opaque bytes behind a header from the profile's closed
tables, its checksum word last, and a body that is the ImageFS raw or
framed as LZO1X or NRV2B chunks of literal and run streams, with the zero
terminator, zero padding and the stored checksum trailer. The tests open,
list, read and verify each prefix and compression value; mutate every
header field, each bound, the framing (a length word or chunk reaching the
trailer, a missing terminator, a short non-final chunk, a chunk decoding
past 65,536 bytes or `imagefs_size`, bytes after a stream's end), the
padding and both sums, each failing with its class and naming `ifsboot`;
check the probe's offsets and ties and the ImageFS signature inside the
startup code; read a `dumpifs -u`-style copy as corrupt; refuse a stack
that names a scheme or another filesystem with `ifsboot`, and a wrap
without the required settings;
fail a body of 1,000 minimum-size chunks before reading past the index
bound or growing scratch past 64 KiB; confine reads to four cached chunks
and stop a five-chunk alternation at the work limit; fail a read of a
chunk whose stream changed after the open; and sweep injected read and
allocation failures through open, verification and reading. `--seed-lzo`, `--seed-ucl` and
`--seed-raw PATH` write the container fuzzer's seeds, and `--emit DIR` the
body and the six images of each prefix and compression value.

```sh
FSIMG_IFS_NATIVE=/path/to/captures-ifs FSIMG_IFS_IMAGES=/path/to/images \
    ./test-ifsboot --native
```

`test-ifsboot --native` reads the E7 `mkifs` compressed captures (LZO,
UCL and UCL with `+compress`, each verified with 2, 2 and 1 entries; the
big-endian one draws no claim), the E10 disk's four 49,184-byte slots
(the unchanged one verifies, the chunk-byte flip fails open and the
stored-trailer and startup-byte flips fail verification, each corrupt),
and, with `FSIMG_IFS_IMAGES`, every compressed boot image there directly,
whose decoded bytes must equal the oracle body in `decoded/`
(`h3-NAME.imagefs` or `x1-NAME.imagefs`) and whose header tail holds six
nonzero bytes in an H3 image and none in an X1 one; an image of another
container there, such as `PCM3_IFS2.ifs`, opens through a spool and is
skipped. It prints counts and the images' file names only. On macOS the
eight H3 images (LZO1X, 120 to 294 chunks) and the three X1 images (NRV2B,
51 chunks) all decode to their oracle bodies and verify.

`test-ifsboot-write` writes boot images through the public API, a create
of `ifs/ifsboot` and a wrap of the same ImageFS from the library's own
ImageFS writer, and checks every byte with its own decoder of the
profile's layout and its own LZO1X decoder, which share no code with
`lib/layers/`: the prefix and its padding, each header field, a zero
`flags2` and tail, the code and its padding, both sums, and the framing,
whose units are each one chunk or two halves. The tests cover both
compression modes; a create and a wrap writing the same bytes; peel,
verification, `i`'s details and the unwrapped sections, from which a wrap
with those values rebuilds the image byte for byte; absent, empty and
padded startup code; the 1,085-byte loader padded to the 1,088-byte
prefix, also through allocations the private poisoning control fills
with 0xa5; incompressible 65,536-byte chunks and a final 65,400-byte one
split in halves; every refusal of the plan, each against a sink that
records writes and resizes; the bound's limits; sections a container
lacks; a file setting aliasing the output; a startup or prefix file
truncated, grown or rewritten between plan and output; a staged volume
or a verified input whose checksum a fault flips; an input that changes
between the two passes; the emission check's fault; allocation-failure,
short-transfer and read-failure sweeps; and digests pinned for every host.
The machine table lists machine 3 under `none` only, so the x86 prefix
under `lzo`, the one mode it does not list, runs under the private
control that lets the plan accept every mode its tables describe.

```sh
FSIMG_IFS_NATIVE=/path/to/captures-ifs FSIMG_IFS_IMAGES=/path/to/images \
    ./test-ifsboot-write --native
```

`test-ifsboot-write --native` unwraps the prefix, startup and disk
sections of each native boot image, checking each against the byte range
the header names, and rebuilds the image from them and the values `i`
reports: the E7 images, machine 0, rebuild under `lzo` identical outside
the body-dependent fields with an identical disk; the uncompressed x86
images, E12's `cAu.ifs`, read from slot 0 of `e12-links/e12-au.img.gz`,
and the x86 boot gate's `e14-gate/gu.ifs`, machine 3, byte for byte; the
gate's `e14-gate/gl.ifs`, `mkifs`'s LZO x86 image, refused under `lzo`
naming its machine; and every H3 and X1 image under `FSIMG_IFS_IMAGES` is
refused naming its machine. It exits 77 without `FSIMG_IFS_NATIVE`.

```sh
./test-ifsboot-write --rebuild IN OUT none|lzo
```

`--rebuild` is the native boot gates' test hook, not an interface: it
unwraps the boot image `IN` the same way, wraps its disk again under the
named mode with its own sections and the header values `i` reports, checks
every byte of the result with the test's independent checker, and writes
it to `OUT`, which must not exist, printing whether it is identical to
`IN`. It runs under the private control that lets a mode the machine table
does not list be written, so that a boot trial can decide the table; such
an image qualifies nothing until it boots. The x86 gate
([research record](ifs-research.md#x86-boot-gate-e14), E14) boots its
output with `tools/qnx/vm/bootifs.sh`.

`make check-integration` runs `test/ifs-integration.sh`, which skips what
is absent:

```sh
FSIMG_IFS_NATIVE=/path/to/captures-ifs ./test/ifs-integration.sh
FSIMG_IFS_IMAGES=/path/to/images ./test/ifs-integration.sh
DUMPIFS='/path/to/dumpifs' QNXPROBE='/path/to/qnxprobe' \
    ./test/ifs-integration.sh
FSIMG_HBCIFS_IMAGE=/path/to/PCM3_IFS2.ifs \
FSIMG_HBCIFS_DECODED=/path/to/captures-ifs/decoded/h3-PCM3_IFS2.imagefs \
    ./test/ifs-integration.sh
```

`test/ifs-native.rb compare` derives the tree fsimg must list from the
manifest the research decoder `tools/qnx/decode/ifsdump.py --manifest`
writes from the record table, by the format design's rules, and compares
it with the command's view both ways. The last record of a path places
it, and the earlier records and every path below a non-directory make the
`shadowed` count; each prefix of a placed path that no record names, and
the root when no record names it, is a synthesized directory, all of them
the `implicit_directories` count; and each placed or synthesized node
whose own name lies outside the name profile while every ancestor's lies
inside is the root of an omitted subtree, all of them the `omitted`
count. `fsimg t --json` must list exactly the placed and
synthesized nodes no omission hides: a recorded one with its last record's
kind, permissions, owner and time, every file the size and POSIX `cksum`
of its bytes and every link the target, both as `test-ifs --manifest`
reads them through the library, which hands the names over a pipe, never
a command line; a synthesized one with the implicit-directory values. An
image fsimg refuses, exiting with status 1 and a corrupt or unsupported
error record, counts as refused, and `test-ifs --native` checks the
library's refusal of each capture against its model; any other failure of
fsimg, the decoder or the library reading, such as a crash or a leak
report, is a mismatch.
The script prints one line of counts per image and a mismatch by category
and count, never a member name, and discards the diagnostics of fsimg,
`test-ifs` and the decoder; an unexpected failure of the comparison prints
only its exception type.

The script writes the six boot images of `test-ifsboot --emit` and
requires each to report the `ifs/ifsboot` stack, verify completely under
`fsimg W -d`, list the same entry records as its bare body and extract the
same tree with `-k`. It also writes two images with `test-ifs`: `--seed`, the fuzzing
seed, whose names outside the profile, control bytes and Latin-1 among
them, are omitted with their subtrees, with implicit parents inside and
outside an omitted subtree and a boot reference; and `--seed-shadowed`,
whose duplicates and paths below a file, a link and a link that shadows a
directory are shadowed. `fsimg W -d` must verify both completely, and when
Python 3 is present the comparison checks itself on them: neither may be
refused, the first must give 4 implicit directories and 6 omitted
subtrees, and the second 2 implicit directories and 5 shadowed records.
With `FSIMG_IFS_NATIVE` it runs `test-ifs --native` and
`test-ifsboot --native` and then the comparison over every bare image in the
`cases/ifs-basic`, `cases/ifs-names`, `cases/ifs-extra`, `cases/ifs-data`
and `decoded` subdirectories. On macOS all 72 bare images agree: 2,620
entries, 1,659 files' bytes and 514 link targets, the odd-paths capture
refused as `test-ifs --native` expects, and nine boot images among the
captures skipped. The whole script takes about 53 seconds, most of it the
decoder's checksums.

`FSIMG_IFS_IMAGES` is the directory the codec integration reads, whose
files may be symbolic links. Each file in it that is a bare ImageFS, or an
uncompressed boot image such as a `dumpifs -u` copy, whose startup header
(at offset 0 or after the 1,088-byte BIOS loader) gives the body's offset
and length, is read in place through `fsimg i`, which must report `ifs`,
and `fsimg W -d`, and the script prints the image's file name with the
verify record's counts only. A compressed boot image is read whole
through `fsimg W -d`, which must report the `ifsboot` container and
verify completely; files that hold neither are counted. `DUMPIFS` and
`QNXPROBE` name an `lclevy/dumpifs` and a `qnxprobe` command at pinned
revisions, split into words, run as `$DUMPIFS IMAGE` and `$QNXPROBE IMAGE`
on the two generated images and every bare capture fsimg verifies; each
must accept them all, and only the number accepted is printed. Without
the variables the three are skips: "FSIMG_IFS_IMAGES is not set (S1 and S2
images, counts only)", "DUMPIFS is not set (lclevy/dumpifs comparison)"
and "QNXPROBE is not set (qnxprobe comparison)"; the decoded S1 and S2
bodies in the captures' `decoded/` are compared above
([TODO](todo-ifs.md)). The generated boot images need neither Ruby nor Python 3
nor a variable, so the script always runs them. The decoder comparison requires
Ruby 3.2 or later and optional `python3`; without either, the script reports
the comparison as SKIP while continuing the independent checks. The script
exits 77 only when no check ran.

`test/ifs-native.rb container` compares an ImageFS inside a container with
the decoder's manifest of an oracle's decode of the container's payload:
the comparison above, with `fsimg t --json` and `test-ifs --manifest`
reading the container through its layers, and then an extraction, `fsimg x
-k -E` into a private temporary directory, in which every placed directory
and file must appear, a file with its last record's size and POSIX
`cksum`, which `cksum(1)` computes from the file on its standard input, and
nothing else; safe extraction creates no links, whose targets the
library comparison checks. The script checks the command on a generated
`hbcifs` container, `test-hbcifs --seed-hbcifs` with the ImageFS it
decodes to (2 entries, one file and one link), and requires it to fail
against another image's decode. `FSIMG_HBCIFS_IMAGE` and
`FSIMG_HBCIFS_DECODED` name `PCM3_IFS2.ifs` from one of the H3 update
disc's `HEADUNIT/ADR*/` directories and its out-of-tree `liblzo2` decode
from the captures' `decoded/`; the script then requires `fsimg i` to report
`ifs/hbcifs` with the `lzo1x` codec, the disk `test-hbcifs --cksum`
decodes to have the decode's `cksum` and length, the container comparison
to agree, `fsimg W -d` to pass with `"complete":false` and `"unverified":
"0x14,0x1c"`, and `fsimg t -l spool=1M` to fail with a spool limit error
whose observed length lies within one 64 KiB step past the limit. On
macOS the image decodes to the decode's 68,849,668 bytes, and lists 364
entries, 214 files' bytes and 120 link targets, with 29 implicit
directories, and extracts 214 files and 30 directories in agreement, in
about 17 seconds. Without both variables the script prints "SKIP:
FSIMG_HBCIFS_IMAGE and FSIMG_HBCIFS_DECODED are not set (PCM3_IFS2.ifs and
its oracle decode)".

`test-ifs-write` creates ImageFS images with the writer, constructed by
name through the registry, which must list it as writable with its byte
order and mountpoint settings. Every image is read back through
fsimg's reader and payload verification, and checked by the test's own
checker, written from [the format design](../specs/design-ifs.md) with
separate offsets and sharing no code with `lib/formats/ifs/`. The checker decodes the header, the mountpoint,
every record and every file's data, sums the image's words and requires
the writer profile's layout: the root record first, inode numbers in record
order, a directory record before every child, every record its kind's
minimum padded to 4, files in record order each on the next 4-byte
boundary, and the trailer last. That gives every byte a checked value, so
over a sink poisoned with `0xa5` any byte the layout gives no meaning must
be zero. The cases build one tree three times, in reverse source order,
with a one-page and a 1024-page metadata cache, with and without scratch
and in another time zone, and require identical bytes; the 128-byte empty
image and the 176-byte one-file image `mkifs` also writes; mountpoints of
0, 1 and 1024 bytes and refusals of 1025 bytes and of bytes outside the
profile; both byte orders; 1024-byte paths, components and link targets
and the 2080-byte record they make; owner and mode limits and time losses;
every refused setting; mutations the checker must
see; sink, source, allocation, metadata-storage and cancellation faults;
and images one word below and at `2^32 - 4` bytes on a sink that keeps only
their first and last 64 KiB, with one word more out of range.

```sh
FSIMG_IFS_NATIVE=/path/to/captures-ifs ./test-ifs-write --native
```

`make check-integration` runs `test-ifs-write --native`. Each image in the
captures' `cases/ifs-basic`, the E1 trees and the E2, E4, E5 and E9 header,
kind, metadata and byte-order cases, is decoded by the checker, its tree is
written with the native byte order, mountpoint and time as the epoch, and
the two images are compared record by record: the header flags, references,
spare words and mount flags, `dir_offset`, the mountpoint, the root, and
every native record's path, mode, owner, time, record length and content.
The writer's other records must be the directory records of the parents
the native table leaves implicit, with the values the reader synthesizes
for them. Inode numbers, record order, data order and alignment, and the
sizes they move, are the free choices the comparison skips. A capture whose
records carry different times, as `mkifs -n` writes them, cannot be
expressed with one epoch and is reported, not compared. On macOS the 26
expressible images match with no difference, adding 9 directory records,
and 5 are not expressible; it prints only counts and capture file names,
and exits 77 without `FSIMG_IFS_NATIVE`.

```sh
./test-ifs-write --create DIRECTORY
```

writes the native qualification images and their manifests into an
existing empty `DIRECTORY`; the plain run builds and checks the same
images without writing them. They are one file, the name and path limits
within and beyond what `dumpifs` shows, every kind, the owner, mode and
time limits, the data sizes and shared contents, the root alone, empty and
1024-byte mountpoints and a big-endian image. Each is checked strictly
against the mountpoint, byte order, entries, metadata and contents that
built it, read and verified before it is written, and the manifest is the
checker's decoding.
`tools/qnx/decode/ifsqual.py` packs them for the QNX 6.5.0 guest's
`tools/qnx/guest/qualifyifs.sh` and compares the capture with the
manifests; [the infrastructure page](qnx6-infrastructure.md) gives the
commands and [the research record](ifs-research.md#fsimg-writer-output) the
result.

`test-codec`, in `make check`, tests the LZO1X and NRV2B decoders on
streams written by hand from [the codec design](../specs/design-codecs.md),
and the LZO1X encoder: its stream for each block the tests build must
equal the stream written by hand from the design's matching and
instruction rules, fit a buffer of exactly the bound `n + n / 255 + 6`,
and decode to the block; incompressible blocks must be one first run;
planted table entries must not change a stream; and a fixed corpus's
streams must have a pinned digest in this process and in a forked one.
`test-codec --native` encodes every 64 KiB chunk of each decoded body in
`$FSIMG_IFS_NATIVE/decoded`, framed as the `ifsboot` writer frames it
(a stream over 65535 bytes splits the chunk in two halves), decodes each
stream again and prints the framed sizes; it exits 77 without the
variable. Its figures are the same on every host: the 12 bodies,
206,596,160 bytes, frame to 3,219 chunks (59 of the 65536-byte chunks
split in two) and 115,721,550 bytes. The H3 bodies frame to 1.13 to 1.15 times their native bodies, which `mkifs`
wrote at the ratio of `liblzo2`'s LZO1X-999, and 0.95 to 0.96 times the
oracle's LZO1X-1 on the same chunks (`PCM3_IFS1`: 10,519,087 bytes
against 9,252,420 native and 11,105,256 under LZO1X-1).
`test-codec --encode IN OUT` writes one file's stream for the oracle, and
`test-codec --encode-framed IN OUT` the file in those framed chunks behind
a minimal startup header that the oracle's `boot` mode reads.
`test/codec-integration.sh`, run by `make check-integration` and not by
`make check`, compares them with black-box oracles that never enter the
tree: `tools/qnx/oracle/build.sh` builds a wrapper around Homebrew's
GPL `liblzo2` 2.10 and `libucl` 1.03 into
`~/.local/share/fsimg-oracles/ifs-codecs/oracle` (`FSIMG_CODEC_ORACLE`
names another path), and without it the script is a skip. The native
images are reached only through the environment:

```sh
FSIMG_IFS_IMAGES=/path/to/images FSIMG_IFS_NATIVE=/path/to/captures-ifs \
    ./test/codec-integration.sh
```

`FSIMG_IFS_IMAGES` is a directory holding, under their original names, the
nine PCM 3.1 images (`PCM3_IFS1*.ifs` and `PCM3_Emergency*.ifs` from the
ISO's `HEADUNIT/ADR*/` directories, and `PCM3_IFS2.ifs`) and the QNX 6.5.0
live CD's `boot/fs/qnxbase.ifs`, `qnxbasedma.ifs` and `qnxbasesmp.ifs`;
symbolic links serve. `FSIMG_IFS_NATIVE` names the captures directory
whose `decoded/` holds the oracle's decoded bodies. Its output goes under
`TMPDIR`. The script checks its self-check table against the oracle and
that a mislisted entry is caught; requires the oracle's verdict and bytes
on every stream `test-codec --emit-cases` writes; compares the two decoders
on `FSIMG_CODEC_COUNT` (300) random strings per codec and three mutations
of each hand-built stream from `FSIMG_CODEC_SEED` (1), by verdict and, on
accept, bytes, never by error code; splits every boot image's chunks with
the oracle's `chunks` mode and requires fsimg's decode of each chunk to
equal the oracle's, the joined body to equal the decoded capture and to
have the `imagefs_size` length and a zero additive checksum (on a
little-endian host), and `qnxbase`'s body to have the `dumpifs -u` copy's
POSIX `cksum` 2731401004 over 3296204 bytes; decodes `PCM3_IFS2.ifs`'s
stream through 64 KiB windows to the header's length and the oracle's
bytes; compares 20 mutations of three chunks of each image; and, with
`FSIMG_IFS_NATIVE`, runs `test-codec --native` and has the oracle decode
each decoded body framed in chunks, split halves included, and encoded
whole as one block, both to the body's bytes. The
hand-built streams include every stream the encoder tests write. With
`FSIMG_CODEC_FUZZ_CHUNKS` naming a directory, those sample chunks are
also copied there for `build-aux/fuzz.sh` to seed the codec corpus. The
chunks are licensed bytes: that directory, and a build directory whose
`codec-fuzz-corpus/` holds them, must stay outside the repository. On macOS every check passes: 309 hand-built streams, 129 of
them the encoder's; 193 random and mutated streams accepted and 1334
rejected by both; 1,966 LZO1X chunks of the eight H3 images and 153 NRV2B
chunks of the three X1 images; `PCM3_IFS2.ifs`'s 33,575,140 stream bytes
to 68,849,668, consumed exactly; 286 mutated corpus chunks accepted and
374 rejected by both; and the 12 decoded bodies, the largest 68,849,668
bytes, each framed in chunks and encoded as one block and decoded by the
oracle to its bytes.
The H3
chunks are compared with the oracle's decodes, not with native `dumpifs
-u` copies, which the serial transport cannot carry
([TODO](todo-ifs.md)). The run takes about 400 seconds of wall time on a
loaded host, most of it process start-up.

The sparse exFAT qualification uses a 64 MiB library budget:

```sh
./test-exfat-scale --huge
```

It creates an actual 128 TiB sparse file, plans an 8 TiB-plus-17-byte zero
source and two 65,537-byte initialized markers, opens and enumerates the volume,
and checks boundary content. Its 8 MiB clusters do not determine transfer-buffer
size. Both APFS and the provided ZFS host pass. The APFS run reports 5,539,528
peak charged memory bytes, 2,097,168 scratch bytes, three owned descriptors,
278,721 written adapter bytes and 36,226,983 read adapter bytes in 0.374 seconds;
process RSS is 21,024,768 bytes. ZFS reports 5,540,296 charged bytes, the same
scratch and adapter counts, 11,882,496 RSS bytes and 0.600 seconds.
Immediate allocated-block samples are 25,321,472 bytes on APFS and 512 bytes
on ZFS; compression and delayed allocation make these samples distinct from
settled device writes. This measures sparse logical capacity and bounded
metadata/content access, not full multi-terabyte payload transfer.
