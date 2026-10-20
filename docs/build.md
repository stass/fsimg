# Building and validating fsimg

Development requires Autoconf, Automake, Libtool, make, a C11 compiler, and a
C++ compiler. On macOS install missing tools with `brew install autoconf
automake libtool pkgconf python`. CLI integration and installation qualification
use Python 3 and pkg-config as development tools. On FreeBSD these are
`lang/python3` and `devel/pkgconf` ports.
Independent FAT tests additionally use `brew install mtools
dosfstools cmake`. Linux exFAT interoperability uses `exfatprogs`.
FreeBSD uses native `newfs_msdos`/`fsck_msdosfs`; install
missing development tools through ports, including `filesystems/mtools`.
The additional fatcat oracle is built from a pinned upstream checkout.
`configure` finds each tool on `PATH`; a tool given to it in a variable such
as `MKFS_FAT` or `FATCAT` must be an absolute path, as the specification tests
treat any other value as an unavailable tool.
Release archives include generated Autotools support and build without those
generators. The installed library has no third-party runtime dependencies.

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
`./test-cli`, and `python3 ../test/cli.py ./fsimg` from a separate build directory
(use `test/cli.py` for an in-tree build). They cover contextual help, exact stream
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
when the suite skips there. The GCC container jobs install no Ruby and skip it.

The tests' base images are cached in `test/spec-cache/` under the build
directory, keyed by the base recipe and the versions of the tools that build
it; `make clean` removes the cache. Measured on macOS (Intel, APFS) with
mkfs.fat 4.2 and mtools, a first run builds the 1440 KiB FAT12 formatter base
in 0.04 s, the 16 MiB FAT16 base in 0.08 s and the populated 64 MiB FAT32 base
in 0.26 s, each including the read-only checker and `mdir`. A cached base is
checked again in 0.02 to 0.20 s, and copying one into a case directory takes
1 to 9 ms. mkfs.fat writes the FAT16 base's zeros, so that base occupies
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
would fail `make distcheck`. Compiler and sanitizer diagnostics name files in the
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
ASan/UBSan and `-fsanitize=fuzzer-no-link` coverage on every object, then compiles
`fuzz/fat.c` with Clang's libFuzzer runtime, and creates seeds for all FAT variants
using the independent fixture builder. A separate exFAT corpus combines an
independent raw-table fixture with a writer-generated compressed-table image.
It runs 1000 mutations per format corpus with a 3 MiB input ceiling
and a ten-second per-input timeout. Each reader has a 16 MiB budget, 1000-entry,
eight-level and 250,000-work limits, and no scratch authority. Each input is
read both as an exact-length byte array and as a prefix of zero-filled logical
storage. The latter reaches FAT32's minimum geometry without a large resident
seed; it does not allocate or transfer the logical volume. The target fuzzes
probe, directory/name parsing, chain traversal, content/range reads, index
construction, and full verification; it also checks cleanup accounting.
`fuzz/qnx6.c` forces the QNX6 decoder and reads each input as the exact slice,
since QNX6 finds its second header from the slice length; its seed is a 64 KiB
B = 512 volume from the independent generator holding every entry kind,
inline and long names and a depth-1 tree, with a 1 MiB input ceiling and the
same budgets.
The same target builds `fuzz/protocol.c` and runs 1000 mutations with a 64 KiB
input ceiling and five-second timeout. It exercises production framing, job
codecs, and typed INFO/PLAN/ENTRY/VERIFY validators. Worker lifecycle ordering
is covered by `test-worker`, not inferred from pure wire fuzzing.
Set `FUZZ_CC` to a Clang with the required runtime; `FUZZ_CXX` defaults to its
sibling `clang++`. Apple Clang lacks the libFuzzer runtime; macOS fuzz
validation uses Homebrew LLVM:

```sh
brew install llvm
FUZZ_CC="$(brew --prefix llvm)/bin/clang" make check-fuzz
```

The exact standalone commands from the top of a separate build directory are:

```sh
make test-fat test-exfat test-qnx6 test-protocol
MAKE=make sh ../build-aux/fuzz.sh
```

Set `MAKE` to the make program that drives the build (`gmake` on FreeBSD); in a
tree configured in place the script is `build-aux/fuzz.sh`. It builds the
instrumented library through `build-aux/sanitize.sh fuzz` and compiles the
harnesses into `sanitizer-fuzz/` from the same read-only source copy.

The target's `fuzz-corpus/`, `exfat-fuzz-corpus/`, `qnx6-fuzz-corpus/`,
`protocol-fuzz-corpus/` and libFuzzer crash artifacts are disposable build
outputs. Existing corpus seeds and mutations are reused; `make clean` keeps the
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

The instrumented fuzz smoke run uses Homebrew LLVM 23.1.2. FAT completes
1000 mutations with FAT12/16/32 seeds (`cov: 2160`, `ft: 5838`), exFAT
completes 1000 mutations (`cov: 2105`, `ft: 5366`), and protocol completes
1000 mutations (`cov: 348`, `ft: 699`). None reports an unintended
sanitizer or accounting finding. These counters describe finite runs and vary
with the generated mutation sequence.

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
read-only OpenCode review through the Z.ai coding-plan subscription. Write a
prompt covering correctness, design, security/information leaks, and the style
guide's readability review, mark new files with `git add -N`, and run from the
checkout:

```sh
build-aux/opencode-review.sh "${TMPDIR:-/tmp}/review.md" prompt.txt
```

The script copies only the tracked files of the working tree into a fresh review
directory under `$TMPDIR` (or `/tmp`), without the checkout's Git metadata,
local configuration, credentials or build products, and adds the change as
`CHANGE.diff` and the prompt as `REVIEW.md`. `CHANGE.diff` is `git diff HEAD`:
every staged and unstaged change to a tracked file is sent, not only the files
meant for review, so keep unrelated or sensitive edits out of the tree. The
copy is initialized as its own Git work tree so that OpenCode looks for project
instructions and configuration only inside it. The script runs `opencode run
--pure --agent plan --model zai-coding-plan/glm-5.3 --variant max --format
json` there with a scrubbed environment: sharing and Claude Code instruction
files disabled, and a fresh private home and configuration, data, state and
cache directories that hold only a copy of OpenCode's credentials; proxy and
CA variables pass through when set. The copy is removed on exit, except after
`SIGKILL`, which leaves the private `fsimg-review-*` directory with the
credential copy behind. Every permission
is denied except `read`, `glob` and `grep`, including access outside the
review directory. Before any model request the script checks the agent's tools
and permissions as OpenCode resolves them (`opencode debug agent plan`), and
refuses to run unless exactly those three tools are enabled and every rule
after the final deny-all keeps access inside the review directory. An attempt
is killed with its process group and retried, three attempts in all, when no
event arrives within 5 minutes of starting, no further event arrives for 20
minutes, or it runs past 90 minutes; `OPENCODE_REVIEW_ATTEMPTS`,
`OPENCODE_REVIEW_STARTUP`, `OPENCODE_REVIEW_STALL` and
`OPENCODE_REVIEW_DEADLINE` override these limits in seconds, from 1 to 999999.
A review is accepted, and written to the output file, only when its final step
stops without tool calls and its text has a body ending with the line `END OF
REVIEW`; otherwise the script exits nonzero. The output file must be a new
name outside the checkout. Each attempt's events and diagnostics are kept beside it as
`OUTPUT.N.jsonl` and `OUTPUT.N.stderr`.

Review substantive fixes again. Report failed or unavailable reviews explicitly.
OpenCode is a development tool and introduces no library runtime dependency.

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
same-executable frontend for fault checks. [libfsimg(3)](libfsimg.3) and the
[checked examples](examples.md) explain embedding and failure handling.

Extraction and worker qualification:

```sh
make test-extract test-worker test-protocol test-sandbox test-sandbox-setup
./test-extract
./test-worker
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

The separate actual-kernel mount fixture needs root only to mount a disposable
alias. Run the test itself as an ordinary user, and always unmount before
removing the fixture:

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
| macOS | `make check-asan` | 27 pass, 4 tool/platform skips |
| macOS | `make check-ubsan` | 27 pass, 4 tool/platform skips |
| macOS | `make check-lsan check-msan` | Unsupported-host SKIP for each, not passing coverage |
| Linux | `make check-asan` | 26 pass |
| Linux | `make check-lsan` | 26 pass |
| Linux | `make check-msan` | 26 pass |
| FreeBSD 16/ZFS | `make check-asan` | 24 pass, 2 tool/platform skips |

Configure searches the standard system sbin directories for formatter/checker
tools. Linux runs dosfstools/mtools and exfatprogs interoperability in its
ordinary and sanitizer suites. macOS and the provided FreeBSD host include
passing independent FAT interoperability. FreeBSD uses its native
formatter/checker plus mtools and fatcat. The real
child ASan fault is intentional; no unintended sanitizer report remains.

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
and automatic sizing margins receive read-only OpenCode reviews with
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
read with `fsimg verify -b -z` at their partition offsets and lengths, also
verify, including the head unit's original populated partition. On a Linux
host as root the script mounts the generated sample with the kernel's qnx6
driver and compares every regular file with `fsimg cat`; elsewhere that
comparison is an explicit skip, and no Linux result is recorded
([TODO](todo-qnx6.md)).

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
