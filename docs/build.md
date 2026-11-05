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

The zstd and xz compressions are optional integrations over the system
libzstd (BSD-3-Clause or GPL-2.0, used under BSD-3-Clause) and liblzma
(0BSD). `--with-zstd` and `--with-xz` each take `auto`, the default,
which uses the library when it links and otherwise says `zstd compression
disabled` or `xz compression disabled` in configure's output; `yes`,
which fails configure without it; and `no` (`--without-zstd`,
`--without-xz`), which never looks. Detection is zlib's: pkg-config
(`libzstd`, `liblzma`), else the header and `-lzstd` or `-llzma`, with the
precious `ZSTD_CFLAGS`, `ZSTD_LIBS`, `LZMA_CFLAGS` and `LZMA_LIBS`
overriding both, and a link test of the functions each profile calls;
for libzstd these include the custom-allocator and frame-header functions
of `zstd.h`'s `ZSTD_STATIC_LINKING_ONLY` section, so a libzstd built
without them counts as absent. liblzma is part of the FreeBSD base
system, whose own libzstd is private, so FreeBSD needs the
`archivers/zstd` port; on Linux install `libzstd-dev` and `liblzma-dev`
(`libzstd-devel`, `xz-devel`), and on macOS Homebrew's `zstd` and `xz`,
whose `zstd` and `xz` tools are also the oracles the compression tests use
(`ZSTD_PROGRAM`, `XZ_PROGRAM`). In a build without a library `fsimg
--formats` lists its compression with no support, a `.zst` or `.xz` name
under `-a` and `--compress` naming it are refused as unsupported with exit
status 1, and its files are not recognized. The found libraries join zlib
in `libfsimg.pc`'s `Libs.private`, and `check-installed-consumers` links
a static consumer with `pkg-config --static --libs libfsimg`, so the check
follows the metadata. CI builds with both libraries on every platform, GCC
16 included, and without them in the GCC 13 job.

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
make check-quick
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

### Build and test scratch on a RAM disk

Build output and test scratch are written and deleted many times, so a RAM
disk is faster than a disk and spares an SSD. Most tests create their
scratch under `TMPDIR`, so a recipe is enough; no test needs changing. On
macOS, which needs no root:

```sh
dev=$(hdiutil attach -nomount ram://33554432)   # 16 GiB in 512-byte sectors
diskutil erasevolume APFS fsimg-ram "$dev"
mkdir /Volumes/fsimg-ram/build /Volumes/fsimg-ram/tmp
export TMPDIR=/Volumes/fsimg-ram/tmp
cd /Volumes/fsimg-ram/build && /path/to/fsimg/configure ... && make -j32 check
hdiutil detach "$dev"    # frees the memory; the contents are gone
```

The disk takes memory only as it fills. macOS will not mount another volume
on it, so a check that mounts images, such as the HFS Plus qualification,
keeps its mount points on the system disk (`qualify.rb --mounts`). It mounts with owners ignored, which
the suite tolerates. Its size must cover the build plus the largest scratch
the chosen target writes: `make check` and `check-scale` fit in 16 GiB, and
`FSIMG_HFS_LARGE=1` writes over 4 GiB of HFS Plus images. The QNX4 writer's
`--scale-limits` run needs about 21 GiB of free scratch space, and the HFS
Plus qualification with `--large-catalog` about 8 GiB, so size the disk for
the target (`ram://50331648` is 24 GiB).

On Linux, `/dev/shm` is a tmpfs on most distributions: use
`TMPDIR=/dev/shm/fsimg-tmp`, and build there only if it is mounted exec. On
FreeBSD a tmpfs mount needs root, so use one on a host you administer.

A test that needs a particular host filesystem keeps its documented
location, such as `QNX4_SCALE_SCRATCH`, and `check-seek-holes` needs a
filesystem that reports holes (APFS and tmpfs do). `build-aux/linux-check.sh`
mounts a tmpfs at `/tmp` in its container, so that check's scratch stays in
memory.

The tree builds warning-free under `-Werror` with Clang and with GCC. CI builds
and checks with Clang on Linux and macOS, and with GCC 13 and GCC 16 on Linux.
The macOS, Linux and FreeBSD jobs run `sh ../build-aux/ci-check.sh` in the
build directory after `make`. Run the same script before a change lands: it
holds the one list of checks, from `make check` through the sanitizers,
`check-scale`, `check-cli-rss` and `distcheck`, and leaves out only the long
`check-extended` and fuzzing targets. The GCC jobs run `make check` and require the CLI suite to pass.
On macOS the Homebrew `gcc` formula installs versioned drivers; configure a
separate build directory with `../configure CC=gcc-<major> CXX=g++-<major>`.

`check-quick` is `check` without the specification harness, its slowest test, and
is the per-commit run; `ci-check.sh` runs the full `check`.

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
sanitizers continue to use the system zlib. No instrumented liblzma or
libzstd exists, so the MSan build configures `--without-xz --without-zstd`
and says so: those profiles are covered by ASan, UBSan, LSan and the fuzz
build, not by MSan. The Linux check image and CI
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

`test-codec-memory` covers the shared codec allocator in
`lib/core/codec-memory.c`: its library-neutral entry points directly (a
`size_t` product that overflows, initialization, zero-byte requests, the
version text), and zlib's binding through real streams. It checks charged
deltas and zero-byte requests,
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

`fuzz/hfsplus.c` exercises automatic probing and forced HFS Plus and HFSX
opens, directory traversal, component lookup, bounded file content and the
name codecs under finite resource limits. `test-hfsplus --seed FILE` and
`--seed-hfsx FILE` generate indexed directories and fragmented files; the
HFSX seed also includes case-distinct names.

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
such seeds as `lzoenc-*.seed`.

`fuzz/lzfse.c` drives the LZFSE and raw LZVN wrappers with exact-size
input allocations and a guarded output buffer. The first byte chooses the
decoder and a capacity through 65536; `test-lzfse --seed DIR` supplies the
block vectors. Its input ceiling is 70000 bytes and timeout ten seconds.
`make test-lzfse && ./test-lzfse` checks every block kind, exact bounds,
truncated prefixes, malformed headers and scratch reuse with encoder-produced
vectors; it needs no encoder at test time.
The same target builds `fuzz/protocol.c` and runs 1000 mutations
with a 64 KiB input ceiling and five-second timeout. It exercises production
framing, job codecs, and typed INFO/PLAN/ENTRY/VERIFY validators. Worker
lifecycle ordering is covered by `test-worker`, not inferred from pure wire
fuzzing. `fuzz/compression.c`, built once per compression the build
implements (`fuzz-gzip`, `fuzz-zstd` with libzstd and `fuzz-xz` with
liblzma), forces that
compression on each input and opens it as a file and, up to 16 KiB, as a
pipe stream, each into a fresh unlinked spool under a 4 MiB limit and a
32 MiB memory budget, so the profile's parsing (gzip's member headers and
trailers), the library's decoder through the codec memory adapter, the
trailing-byte rule and the spool reservation see every mutation, with a
256 KiB input ceiling. `test-compression --seed-NAME DIRECTORY` writes
each corpus's seeds: for gzip a two-member file with FEXTRA, FNAME,
FCOMMENT and FHCRC, for zstd frames at several levels, with and without
a checksum and a content size, concatenated and behind a skippable frame,
and for xz streams at several presets and checks, concatenated, padded,
multi-block and unchecked.
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
make test-fat test-exfat test-qnx6 test-qnx4 test-hfsplus test-ifs test-codec \
    test-protocol test-layers test-containers test-compression test-mtree
MAKE=make sh ../build-aux/fuzz.sh
```

Set `MAKE` to the make program that drives the build (`gmake` on FreeBSD); in a
tree configured in place the script is `build-aux/fuzz.sh`. It builds the
instrumented library through `build-aux/sanitize.sh fuzz` and compiles the
harnesses into `sanitizer-fuzz/` from the same read-only source copy.

The target's `fuzz-corpus/`, `exfat-fuzz-corpus/`, `qnx6-fuzz-corpus/`,
`qnx4-fuzz-corpus/`, `hfsplus-fuzz-corpus/`, `ifs-fuzz-corpus/`,
`codec-fuzz-corpus/`,
`protocol-fuzz-corpus/`, `image-fuzz-corpus/`, `container-fuzz-corpus/`,
`plist-fuzz-corpus/`, `gzip-fuzz-corpus/`, `zstd-fuzz-corpus/`, `xz-fuzz-corpus/`, `mtree-fuzz-corpus/` and libFuzzer
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
Scratch on a RAM disk is fine for the check: it takes system memory outside
the library heap budget, which these measurements do not count.

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
volumes: every live inode listed and verified, charged memory at most
5,535,848 bytes (the metadata cache's maximum, one sort run and 256 KiB of
other state) at every size, scratch within a context budget linear in `N`,
the inode count, the live inodes and the blocks objects own, no owned block
read twice by one operation, and the system run read in bounded passes, so
that only
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
verified; charged memory at most 5,535,848 bytes (the metadata cache's
maximum, one sort run and 256 KiB of other state) and scratch within a
budget the context enforces (the claimed bitset of `N` bits, 128 bytes per
name and 32 per extent), which together keep the 256 MiB claimed bitset
out of memory; the operation's growth of the child's peak RSS at most 32 MiB; every
extent block read exactly once by each operation; directory reads at most
each directory's slots once, the root's twice, and each name's record once
more; the bitmap read in full only by verification, once, beside one
window load per 4 KiB window holding owned blocks and two per extent block;
and the `.inodes` table read in full only by verification. On APFS:

| Case | Operation | Image calls | Image bytes | Bitmap bytes | Directory bytes | Extent-block bytes | Peak scratch | RSS | Seconds |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `N` = 2^31 | list | 15 | 38,594 | 12,800 | 16,512 | 0 | 268,435,472 | 4,329,472 | 0.003 |
| `N` = 2^31 | verify | 65,553 | 268,482,242 | 268,448,256 | 16,512 | 0 | 268,443,724 | 4,395,008 | 2.218 |
| 200,000 names | list | 206,261 | 25,633,973 | 4,608 | 25,616,960 | 0 | 21,208,439 | 6,746,112 | 1.951 |
| 200,000 names | verify | 206,264 | 25,646,193 | 8,636 | 25,616,960 | 0 | 21,208,439 | 6,881,280 | 2.257 |
| 65,535 extents | list | 79,838 | 39,753,244 | 5,614,080 | 16,448 | 559,616 | 0 | 3,993,600 | 0.212 |
| 65,535 extents | verify | 68,835 | 39,778,469 | 5,631,113 | 16,448 | 559,616 | 0 | 4,091,904 | 0.214 |
| 16,383 levels | list | 32,777 | 9,465,412 | 3,072 | 9,453,056 | 0 | 0 | 3,678,208 | 0.183 |
| 16,383 levels | verify | 32,780 | 9,476,164 | 5,632 | 9,453,056 | 0 | 0 | 3,727,360 | 0.188 |

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
1,810,441 and 5,425,545 bytes, the deep tree's walk stack of 32 bytes per
level of `max_depth` included; the 200,000-name directory's scratch peak is
its sorted key records, about 106 bytes per name. The RSS column is the
child's peak, which includes the pages it inherits from the test process.
The bound applies to the operation's growth over the resident set the
child starts with; on FreeBSD that baseline is read from the current
resident set, since `ru_maxrss` stays zero until the first statistics-clock
tick while fork already counts the parent's resident pages in the child's.
Memory the child allocates in pages it inherited resident does not raise
its resident set, so on FreeBSD and Linux the growth is a lower bound,
which can fall below charged memory; it still catches a resident claimed
bitset. Ordinary `test-qnx4` runs the same cases in memory at 100,000
blocks, 2,000 names, 2,000 extents and 255 levels, the file at the default 256-component depth
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
6,060,136 bytes (the metadata cache's maximum, one sort run and 768 KiB of
other state), the child's RSS growth beyond the sink's pages at most 64 MiB, and
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
5,535,848 bytes (the metadata cache's maximum, one sort run and 256 KiB
for the reader's other state); scratch within a budget the context
enforces, four times the table and 64 bytes per entry; the operation's
growth of the child's peak RSS at most 32 MiB, below the million-record table; the million-record
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

The HFS Plus work uses the hfsfuse `hfsdump` reader as an independent oracle,
built the same way from a pinned upstream revision:

```sh
sh build-aux/build-hfsfuse.sh /tmp/fsimg-hfsfuse-oracle
export HFSDUMP=/tmp/fsimg-hfsfuse-oracle/bin/hfsdump
```

The destination must be new, so remove it to retry. The helper needs Git, a C
compiler and GNU make, which is `MAKE=gmake sh build-aux/build-hfsfuse.sh ...`
where `make` is BSD make, builds only the `hfsdump` target, which carries its own libraries and needs no
FUSE library, and verifies revision `eed780cbe0445893de6f4aa8d74461da00756984`.
The Linux CI job and the `build-aux/linux-check.sh` image build it and
export `HFSDUMP`, and install the `hfsprogs` and `sleuthkit` packages. The
[HFS Plus research record](hfsplus-research.md) holds the native tool
identities and the unprivileged procedure for attaching a raw image on macOS.

Run the reader comparisons from a build directory without generating the writer
matrix:

```sh
FSIMG_HFS_ONLY=reader \
FSIMG_HFSPLUS_NATIVE="$HOME/hfsplus-captures" \
FSIMG_HFSPLUS_CORPUS=/path/to/hfsplus-corpus \
    sh ../test/hfsplus-integration.sh
# Equivalently, supply the generator and fsimg paths explicitly:
FSIMG_HFS_ONLY=reader ruby ../test/hfsplus-integration.rb \
    ./hfsplus-write-gen ./fsimg
```

The reader selects raw native captures at `$FSIMG_HFSPLUS_NATIVE/*/*.img` and
`$FSIMG_HFSPLUS_CORPUS/linux-mkfs-hfsplus/*.img`; it does not unpack archived
captures. Each absent source is named as a skip. It also creates a sparse 16 MiB
volume with `mkfs.hfsplus` when available. With both source variables unset,
only this local mkfs source is considered by the reader. Omitting
`FSIMG_HFS_ONLY=reader` also runs the writer matrix. Startup records the hfsfuse
build pin (hfsdump has no version flag), `fls -V`, and `uname -sr`. Oracle
comparisons use the command and per-volume bounds described below.

The verification manifest, `test/hfsplus-verify-expected.tsv`, runs under
`test/cli/hfsplus_verify_manifest_test.rb` with `FSIMG_HFSPLUS_NATIVE` naming the
native captures, `FSIMG_HFSPLUS_CORPUS` the real-world specimens and
`FSIMG_HFSPLUS_DECODED` a directory of images from containers fsimg does not
read. The one such specimen is `mozilla-1.0-smi.img`, a self-mounting NDIF image.
Mozilla's release is fetched by hand, its corpus record gives the source URL,
and decoded locally:

```sh
ruby tools/hfsplus/ndif.rb mozilla-macosX-10.smi.bin \
    "$FSIMG_HFSPLUS_DECODED/mozilla-1.0-smi.img"
shasum -a 256 "$FSIMG_HFSPLUS_DECODED/mozilla-1.0-smi.img"
# ab850b44d69d876cb63d9690997605e9e6e9b3dbb73933b7856b111f5a206b8a
```

The input's SHA-256 is
`bb7197fbcb10a24d4f09978443a827a63d1e5d39cb8ee3e4e80371e9528726ca`. Neither file
is redistributed; both stay local. The image is untrusted and is only ever read
by fsimg as a file, never attached or mounted.

`./test-fat --volume-labels` covers native formatter labels with the archive
bit, malformed label placement/allocation/attributes, and verification's
agreement of boot-sector and root labels. Automatic FAT sizing
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

`test-hfsplus-codec` decodes hand-laid HFS Plus records at the format design's
offsets, at every truncated length and with hostile framing fields.
`test-hfsplus-unicode` computes the three pinned table digests through the
driver's lookups, compares decomposition with a compiled-in subset of the native
E3 vectors, and checks the exchanges, name admission, comparators and
namespace keys. Neither reads a capture at run time or writes a file.
`test-hfsplus-salvage` extracts generated volumes under salvage into a private
fixture: clean and damaged compressed files, broken overflow walks, truncated
volumes and a rejected listing, with a plain sibling proving the walk went on.

`test-hfsplus-faults` sweeps allocation, scratch open/resize/read/write, short
positive source reads, premature EOF, source errors and cancellation separately
through open, walk, content and reopened payload verification. Its generated
indexed catalog includes overflow extents, attributes, inline and resource-fork
zlib content, and a journal. Bounded metadata caches under a 16 MiB context
budget force scratch use in each operation, asserted separately from setup.
The public reader exposes decoded file content;
raw resource forks and attribute values have no public read API. Preparation
and verification validate those records. Short reads must match clean entry counts, and in the content stage content
digests. Allocation faults run each point twice, with and without a diagnostic
record; other faults alternate. A failure given a record must report its status
there. The count of open descriptors must be unchanged at the end. Every attempt checks memory, scratch, descriptors
and the private scratch directory after teardown, then opens and walks again
on the same context. Run `./test-hfsplus-faults` in the build directory;
optional stage and fault names select a single sweep, for example
`./test-hfsplus-faults content allocation`.

`test-hfsplus-scale` generates every image byte from its address, retaining one
4096-byte B-tree node and layout integers, with no image file or resident record
catalog. It opens through callback I/O under a 16 MiB memory budget, walks all
entries, reopens for verification, which reads payload for the extents shape
and metadata alone for the others, and checks memory, scratch and descriptor
accounting against the live context's baseline after reader and I/O teardown.
The walk's entries must match an order-independent sum of path hashes computed
by one pass over the generator's catalog ordinals. The count of open descriptors must be unchanged at the end. Each process creates and removes a private scratch directory
beneath its working directory. Run `./test-hfsplus-scale --catalog 10000000`, `--wide 100000`,
`--extents 1000000`, `--deep 256`, `--depth-limit`, or `--capacity` in the build
directory. Catalog files are grouped under folders of at most 2048 children;
the wide directory must spill beyond the memory budget. The fragmented file has
512-byte extents separated by free blocks, with eight inline extents and computed
overflow records; sequential reads compare every payload byte and reverse-order
samples straddle extent boundaries. The capacity fixture has 4,294,967,295
4096-byte allocation blocks, a file named `hello` in the last usable block, and
a 536,870,912-byte bitmap fork. Verification must read every bitmap byte in order.
`make check-scale` selects one million catalog files and the qualification sizes
of the other cases; `make check-extended` selects ten million catalog files.
The catalog qualification exceeds a minute on the measured host. RSS may grow
by at most 32 MiB above the pre-open peak, allowing the charged budget plus
16 MiB of runtime and allocator overhead. Metadata block reads are bounded per shape to catch repeated whole-tree scans,
as walk and verification passes over the metadata plus 150 fixed reads: catalog
and wide 1 and 3, extents 2 and 4, deep and depth limit 21 and 23, capacity 1
and 2.

Reference macOS x86_64 measurements follow; the two extents rows come from one run, the others from an earlier one. Paired values are walk / verify;
resource sizes and RSS are bytes, metadata reads count delivered 512-byte sectors
of the catalog, overflow tree and bitmap, and pass equivalents divide by the
number of those sectors. Seconds include opening the reader and, for the extent
walk, reading and checking all payload; extents verification reads payload too.
Each profile runs in a separate process.

| Case | Walk entries | Source calls | All I/O calls | Metadata sector reads | Metadata passes | Peak scratch | Peak charged memory | Peak RSS | Seconds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| catalog-1000000 | 1000489 | 73961 / 362128 | 73961 / 467720 | 591723 / 2897045 | 0.513 / 2.513 | 144112012 | 5363280 | 14368768 | 5.296 / 11.077 |
| catalog-10000000 | 10004883 | 739416 / 3620921 | 739416 / 4852450 | 5915363 / 28967394 | 0.513 / 2.513 | 1441003696 | 5363280 | 11894784 | 22.908 / 62.073 |
| wide-100000 | 100000 | 7158 / 35972 | 17235 / 54860 | 57299 / 287800 | 0.497 / 2.497 | 24612794 | 5375825 | 10358784 | 0.392 / 0.808 |
| extents-500000 | 1 | 501396 / 503829 | 507574 / 533168 | 10131 / 30658 | 0.978 / 2.960 | 36013218 | 5428832 | 10407936 | 0.683 / 1.327 |
| extents-1000000 | 1 | 1002646 / 1007630 | 1014795 / 1078007 | 20131 / 61065 | 0.973 / 2.952 | 72013410 | 5428832 | 10432512 | 1.367 / 3.024 |
| deep | 256 | 813 / 899 | 813 / 899 | 6539 / 7212 | 20.371 / 22.467 | 0 | 1162364 | 3956736 | 0.004 / 0.007 |
| depth-limit | 256 | 816 / 901 | 816 / 901 | 6563 / 7236 | 20.445 / 22.542 | 0 | 1162424 | 3809280 | 0.004 / 0.005 |
| capacity | 1 | 11 / 131090 | 11 / 131090 | 110 / 1048742 | 0.000 / 1.000 | 0 | 1160209 | 2490368 | 0.001 / 3.531 |

The deep fixture has 255 nested folders and a bottom file: 256 entries at
`max_depth = 256`, with successful walking and verification. `--depth-limit`
has 256 folders and a bottom file at depth 257. It asserts `FSIMG_ELIMIT`,
`FSIMG_RESOURCE_DEPTH`, limit 256, observed 257, after delivering 256 folders,
including cleanup and an incomplete verification report. Both cases assert
the metadata-pass and RSS bounds above. Linux and FreeBSD measurements are
not included.

For the extent comparison, verification reads payload at both sizes. Doubling
from 500,000 to 1,000,000 extents increases verification source reads from
503,829 to 1,007,630 (2.00 times) and metadata block reads from 30,658 to
61,065 (1.99 times), and charged memory stays at 5,428,832 bytes. Verification
I/O calls increase from 533,168 to 1,078,007 (2.02 times) and time from 1.327
to 3.024 seconds (2.28 times). The scratch I/O within those calls grows from
29,339 to 70,377 (2.40 times), since sorting needs another merge level; source
reads remain proportional to the extent count.
For one million versus ten million catalog files, verification source reads grow
from 362,128 to 3,620,921 (10.00 times) and metadata passes stay at 2.513.
Total verification I/O grows from 467,720 to 4,852,450 (10.37 times), including
scratch I/O across merge levels. Peak charged memory remains 5,363,280 bytes.

`test-hfsplus-write` plans, packs and emits through the writer of
`fsimg_hfsplus_format`: round trips through the reader and the verifier,
an independent raw checker (`test/hfsplus-write-check.c`) written from the format
design, byte identity across source order, cache size, scratch path and time
zone, a scan of every unused byte with the transfer buffer poisoned, and faults at
every stage. Volumes stay in memory, apart from a few publication cases.

`test/hfsplus-integration.rb` generates writer output with `hfsplus-write-gen`
in a temporary directory and runs each oracle it finds, naming each one it
skips: `fsck_hfs -fn` on macOS through the attachment owner in
`tools/hfsplus/capture.rb`, which attaches with `-nomount` and always detaches;
`hfsdump`, from `$HFSDUMP` or `~/.local/share/fsimg-oracles/hfsfuse/bin/hfsdump`;
the Sleuth Kit's `fsstat`, `fls` and `icat`; and `fsck.hfsplus -n`. Any `fsck_hfs`
line other than its progress lines fails the script, and
`FSIMG_HFS_ORACLE_LOG` keeps the whole output. The script covers both variants at
every block size, the smallest volumes, and exact sizes whose alternate header
straddles the last block or lies past it. `FSIMG_HFS_LARGE=1` adds 1 GiB volumes
at 4096 and 8192, catalogs with map nodes, and nearly full volumes; it writes
over 4 GiB of images, then removes them.

Its reader comparisons use `fsimg W --json` and `fsimg t --json` to establish
one baseline for both captured and writer volumes. Verification must complete
or match the named refusal in `test/hfsplus-verify-expected.tsv`, including its
status and message; successful rows also check the five block and entry counts.
One `fsimg x --json -E -k` extracts the whole volume into a new private directory.
Every regular file is hashed from disk with a streaming SHA-256 digest and its
size checked against the listing. Symlink targets use individual `fsimg x -O`
commands. The extraction summary must account for all listed symlinks, fifos, devices and sockets
as skipped entries and omit no names. The extraction directory is removed before
any independent reader runs.
Native manifest hashes and symlink targets are checked when present, including
members absent from the fsimg digest set.

hfsdump stats every addressable listed path and the root, compares types and logical sizes,
and uses each folder's `valence` to decide whether to read its children. Missing
valence is a finding. A non-root folder with positive valence but no fsimg
children is a finding; root may count hidden journal and private objects.
Zero-valence folders are not read because hfsdump hangs on them. File bytes and
symlink targets are compared by streaming SHA-256 and byte count. Sleuth Kit
runs `fsstat`, compares recursive `fls -r -p` paths and types, and streams `icat`
bytes into the same digest check. Name comparisons preserve multiplicity,
including collisions in fls's `^` spelling of stored slash and NUL. hfsdump
prints a name through `puts`, which stops at a stored NUL, and cannot address
that name by any spelling. A member with U+2400 anywhere in its path receives
a named skip. A folder's listing leaves out each child name holding U+2400, and
each such name must account for exactly one line no other name claims. Sleuth Kit skips map-node
catalogs because `fsstat` is too slow on them. For map-node shapes, hfsdump
compares the root listing against all entries and reads one synthetic member.
It prints a `CAP hfsdump` line because a process per member takes over 30 minutes
on this catalog. Writer tree checks also require
the fixture's root names and hello bytes, or the empty synthetic map-node file.

hfsdump and Sleuth Kit each have a 60-second budget per volume. fsimg has 60 seconds per command. A timeout
in fsimg or any command on a local volume is a finding. An independent oracle
timeout on a non-local volume is a skip unless earlier member findings exist;
those findings and the timeout are reported together. Diagnostics state the
actual remaining budget at command start, or that the volume budget ran out.
Command stderr and failure excerpts retain only a few KiB even for noisy tools.
Each oracle and volume emits a `COMPARE` line with entry, file and byte counts
and an agreement, finding or skip; `SUMMARY` counts every comparison row and
explicit sources with no images as findings.

The script exits 1 for findings or when an explicitly set
`FSIMG_HFSPLUS_NATIVE` or `FSIMG_HFSPLUS_CORPUS` matches no images. It exits 77
when no independent oracle completed an agreement: a fsimg baseline row alone
proves nothing. Otherwise it exits 0. `test/hfsplus-integration.sh` first runs
the capture and integration harness unit tests without images or device access,
and `test/hfsplus-qualify-test.rb`, whose native case on macOS takes the smallest
empty volume through the qualification cycle. The whole cycle, which runs on
macOS with `hdiutil` and only on the images it generates, is:

```
ruby tools/hfsplus/qualify.rb --generator ./hfsplus-write-gen --fsimg ./fsimg --root DIR
```

It exits 1 unless every image passes, and leaves `results.json` under `DIR`.
Build `all` before asking for `hfsplus-write-gen`, in a separate `make`: `all`
recurses, so naming both in one parallel run links the libraries twice at once
and corrupts them.
`--only REGEX` runs part of it, and `--large-catalog`, which takes hours, runs the
one volume whose catalog passes `2^32` bytes. `--root` and `--mounts` must be
directories private to the user. `--root` may be on a RAM disk, but macOS
refuses to mount on a volume a user attached, so then pass `--mounts DIR`
naming a directory on the system disk for the mount points, which hold no
data.

The run on Darwin 25.6.0 against the native captures, with the corpus variable
unset, used hfsdump at the hfsfuse pin `eed780cbe0445893de6f4aa8d74461da00756984` and Sleuth Kit 4.15.0:

| Source | Volumes | Oracle | Result | Known limitations |
| --- | ---: | --- | --- | --- |
| Native raw captures | 44 | fsimg | 35 agree, 9 expected refusals | Classic HFS, HFS wrappers, journal replay and journaled directory hard links |
| Native raw captures | 44 | hfsdump | 34 agree, 10 skip | 9 unavailable baselines; 1 ditto volume has incorrect type 8 resource bytes |
| Native raw captures | 44 | Sleuth Kit | 28 agree, 16 skip | 9 unavailable baselines; 7 volumes encounter attribute, thread-position or compression limitations |
| Local mkfs | 0 | All | Skip | mkfs.hfsplus unavailable |
| Writer volumes | 32 | fsck_hfs (version hfs-715.160.9) | 32 agree | None |
| Writer volumes | 32 | fsimg | 32 agree | None |
| Writer volumes | 32 | hfsdump | 6 agree, 26 skip | Each tree volume holds the stored-NUL name, which hfsdump cannot address; its other members are compared |
| Writer volumes | 32 | Sleuth Kit | 30 agree, 2 skip | The small nearly full volumes hit the thread-position limitation |

This totals 197 agreements, 63 skips and no findings over 92,190,307 compared
bytes. No capture oracle timed out.
Linux and FreeBSD reader runs are not recorded here.

The frozen oracle limitation table is restricted to observed names and errors.
A member limitation applies only after that member's fsimg digest matches the
native manifest. Without a manifest match there is no member exception.
A volume-wide limitation has no member digest and matches on volume name and
error alone; it may name a writer volume too. Its row is a skip, so it never
counts as an independent agreement, but a skip does not fail the run, so the
entry pins its exact diagnostic. On the writer volumes hfsdump, when present,
still lists the affected folder.
TSK exposes synthetic system forks, private HFS+ storage and journal objects
that fsimg intentionally omits. TSK cannot read the fork-backed attribute on
`/attr-sizes` in both 16 MiB e5 captures. fls gives up on a folder whose thread
record starts in the last 520 bytes of its catalog node, omitting `/u1/a` in
the 8 MiB HFS Plus e4 capture and `/dir/sub/below` on the small nearly full writer
volumes. The 8 MiB HFSX e4 capture fails catalog thread lookup. The HFS Plus and
HFSX e6-lzfse captures encounter unsupported inline/resource LZFSE decoding on
`forced-mixed-196608`, `forced-random-1`, `mixed-196608`, `random-1`,
`shape-{131073,262145,393217,589825}`, and
`{text,zeros}-{1,65535,65536,65537,200000}`. TSK misreads LZVN resource chunk
bounds on `forced-mixed-196608`, `mixed-196608`, `text-200000` and
`zeros-200000` in the HFS Plus e6-lzvn capture. hfsdump returns incorrect bytes
for `shape-{262145,393217,589825}` in the HFS Plus e6-ditto capture; fsimg's
digests match the native manifest. These limitations produce skips rather
than agreements, while other members continue to be checked. Unknown
disagreements remain findings.

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

## ISO 9660 oracles

ISO producers and consumers are development tools, never dependencies of
libfsimg. On macOS install the external families with:

```sh
brew install xorriso cdrtools sevenzip qemu
# Optional process oracle in a virtual environment.
python3 -m venv "$HOME/.cache/fsimg-iso/pycdlib-venv"
"$HOME/.cache/fsimg-iso/pycdlib-venv/bin/pip" install pycdlib
```

`/usr/bin/bsdtar` and `hdiutil` are supplied by macOS. Reference measurements
use xorriso 1.5.8.pl02 (libisofs 1.5.8), cdrtools `mkisofs`, `isoinfo`
and `isovfy` 3.02a09, `/usr/bin/bsdtar` 3.5.3 with libarchive 3.7.4,
7zz 26.03, and QEMU 11.1.2. SeaBIOS is
`/usr/local/share/qemu/bios.bin`; OVMF is
`/usr/local/share/qemu/edk2-x86_64-code.fd` with Homebrew's `/usr/local`
prefix. These tools run as external programs; no implementation code or
producer image enters the repository.

On Linux, `apt-get install genisoimage xorriso libarchive-tools p7zip-full`
supplies additional oracles. The required cdrtools listing and verification
oracles are built with `build-aux/build-cdrtools.sh NEW-DIRECTORY`; put its
`bin` directory first in `PATH` before configuring. The builder verifies a
pinned source archive and retains upstream source and licensing outside
the repository. Cdrkit's `isoinfo` from genisoimage cannot follow Rock Ridge
relocation even in its own producer's images, so it cannot replace this
qualification tool. Genisoimage remains an independent producer. FreeBSD supplies `makefs` and `mount_cd9660` in base;
`pkg install xorriso cdrtools libarchive 7-zip qemu` supplies the other
families. Linux can use the cd9660 backend of kusumi/makefs, built outside
the repository from its upstream checkout. Pycdlib is optional; the generator
records it as an unimplemented producer independently of its installation.

Generate the corpus in a new directory whose parent already exists:

```sh
mkdir -p "$HOME/.cache/fsimg-iso"
sh test/iso9660-corpus.sh "$HOME/.cache/fsimg-iso/corpus"
```

The script uses Ruby 3.2 or later (`RUBY` selects the interpreter). It
refuses existing destinations and destinations inside the repository,
including symlink aliases. All sources are synthetic and live under the
private output directory. The CLI sets its working directory and `TMPDIR`
beneath that root and disables core dumps before running tools. Producers
receive that directory as `HOME`, with `MKISOFSRC` and `GENISOIMAGERC` unset
and `SOURCE_DATE_EPOCH=946684800`. Readable empty `.mkisofsrc` and
`.genisoimagerc` files in the private working directory stop executable-directory
rc fallback. Xorriso receives `-no_rc` before its
mkisofs emulation options. Executable paths are resolved through symlinks
and deduplicated, and version output selects the mkisofs option dialect.
Each external tool has a ten-minute timeout. `images.jsonl` records the
image name, producer,
version, shell-quoted command, output path and SHA256. Producers stream to
a sparse sink; each 4,294,971,396-byte source and its ISO occupy only a
small fraction of their logical size. Source file hashes are cached across
producers and views. The commands are an invocation
manifest in `test/iso9660-corpus.rb`; the shell entry point and helper are
in `EXTRA_DIST`, and corpus generation is not part of `make check`.

Each image has a source JSONL manifest and, for the views it offers,
`primary`, `rockridge` and `joliet` JSONL manifests recording path, type,
size, link target and content SHA256. For files, section counts come from repeated
paths in `isoinfo -f`; `-l` combines their sizes. The large fixture's expected
digest always comes from its uniquely corresponding source file; the
observed isoinfo SHA256 and byte count remain separate fields. Other files
require an exact extracted byte count. Raw `isoinfo -l` and `-f` listings,
separate stderr logs, descriptor reports, producer logs and default-view bsdtar mtree manifests
remain beside the image. The latter include SHA256 and link targets
without extracting special files. View status files identify oracle
limitations; `.mtree.status` distinguishes a complete libarchive manifest
from a partial one. `.iso.status` records typed consumer failures per image;
`skips.txt` records missing tools, failures and skipped cases.
Directory sizes in view manifests are the on-disc sizes, whereas source
manifests use zero for directories and special files other than symlinks.
Both representations give a symlink its target's byte length. Names and hashes in
these files concern synthetic data only.

The xorriso, mkisofs and genisoimage invocations cover empty and minimal images,
interchange levels 1 to 3, Rock Ridge files/directories/symlinks/FIFOs,
238-byte names using SUSP continuation, a 20-directory deep tree,
Joliet, multi-extent files, BIOS/EFI/both catalog entries, and visible and
hidden catalogs. Character and block device fixtures are omitted to avoid
creating host driver endpoints, including when run as root. Bsdtar supplies
another producer
family for the small source trees; its multi-extent production is skipped
because libarchive stages payload in a temporary file, defeating the
small-disk requirement. Native producers emit Joliet level 3 (`%/E`);
levels 1 and 2 have no selector and are explicitly skipped. Normal and
long Joliet cases exercise truncation at 64 and 103 UTF-16 units;
xorriso `-joliet-utf16` supplies a non-BMP surrogate pair. Xorriso supplies
an MBR/GPT partition-table hybrid with `-part_like_isohybrid` and
`-isohybrid-gpt-basdat`; it has no Syslinux MBR boot program. Mkisofs and
genisoimage isohybrid variants require an external Syslinux MBR and are
skipped. The all-zero BIOS and EFI payloads are structural catalog
fixtures, not bootable applications or a FAT EFI system partition. A
separate 606-byte symlink-target probe makes mkisofs 3.02a09 emit malformed
split SL data: isoinfo diagnoses a bad entry version and bsdtar reads a
truncated target. This optional long-link probe stays outside the corpus;
the required long-name NM continuation fixtures are retained.

The owner's H3 image is opened read-only at
`~/PCM-3.1-v4.76-disc-image.iso`. It is never copied, extracted, or included
in the corpus. A counts-only survey uses `isoinfo -d`, plain `-f`,
`-R -f`, `-J -f`, xorriso `-report_el_torito plain`, and the raw-byte
evidence reader below. No member names are retained.
The descriptor types, in order, are **1, 2, 255** (primary, Joliet,
terminator). There is Joliet level 3, no SUSP/Rock Ridge and no El Torito
catalog. Both hierarchies contain **7,763 files and 2,204 directories
including the root** (2,203 below it); each `-f` listing has 9,966 entries.
The maximum directory depth is **10 below the root** (11 including it),
and the maximum file path has 11 components. There are **zero
multi-extent files**. Descriptor and record sequence numbers are 1,
Joliet volume flags are 0, and recording and descriptor dates are valid
or absent. Its directory records, dot records included, give each
directory's data length as the exact byte count of its records rather
than a multiple of 2048 (`isoinfo -l` lists 6,611 directory records, none
a multiple of 2048), and the bytes after the last record of each block are
zero; the [framing rule](../specs/design-iso9660.md#framing-rule) accepts
both.

The evidence questions in [the ISO plan](iso9660-plan.md#evidence-questions)
have the following answers. Producer measurements use the versions above;
consumer claims do not imply qualification of the fsimg writer.

| Question | Evidence |
| --- | --- |
| RRIP_1991A with 36-byte PX | Xorriso 1.5.8.pl02 writes this pair. Mkisofs 3.02a09 and libarchive 3.7.4 write `RRIP_1991A` with 44-byte PX, including explicit mkisofs `-rrip112`. |
| Empty file extent 0 | Xorriso writes block 32 in the small fixtures; mkisofs and libarchive write `0xfffffff0` for the ordinary empty file. H3 writes 0. Zero-length records therefore need no extent range check. |
| Multi-extent Rock Ridge entries | Xorriso and mkisofs level 3 split the 4 GiB source into two sections. Xorriso repeats `PX`, `TF`, `NM` on both; mkisofs repeats `RR`, `NM`, `PX`, `TF` on both. Bsdtar reads the complete content. |
| EFI sector count and OVMF | Xorriso's EFI entry for the 1,474,560-byte fixture has load count 2,880, its size in 512-byte sectors. |
| Directory and path-table order, including ties | The synthetic images have zero order inversions under a raw decoder using padded name/extension comparison, descending file versions and equal-order ties. The `FOO` directory and `FOO.;1` file exercise a tie. H3 primary has no inversions, but Joliet has 274 adjacent directory-record inversions and 56 path-table inversions in each L/M copy. Owner decision 8 requires strict primary order; Joliet order is not checked. |
| Sequence numbers, SP skip, continuation boundaries, dates, Joliet flags | Xorriso, mkisofs, libarchive and H3 use sequence 1; the Rock Ridge producers use SP skip 0; their observed continuation areas stay within one 2048-byte block. Joliet flags are 0. Descriptor and recording dates are valid or absent; absent descriptor dates use ASCII zero digits. This is a finite corpus observation, not evidence that all external images obey these bounds. |
| Relocation placeholder shape | Xorriso and mkisofs CL placeholders have flag 0, directory PX mode `040755` and zero data length. Xorriso uses extent 43; mkisofs uses sentinels `0xffffffee`, `0xfffffff0`, `0xffffffef`. The placeholder's extent does not locate its directory: CL does. Libarchive's deep-tree producer crashes with signal 11; the failed file is excluded from the image manifest. |
| Relocated logical tree and relocation-directory visibility | Isoinfo follows the xorriso tree; `-R -l` includes placeholder directory rows which `-R -f` omits. Libarchive 3.7.4 fails to connect the xorriso CL/RE pointers and leaves a partial mtree with nonzero status. |
| Joliet 62-unit files and 64-unit directories | The producers emit these lengths. Xorriso and mkisofs omit `;1` from Joliet file identifiers. Isoinfo is lossy for non-BMP UTF-16 names even with `-j utf-8`; bsdtar reads the surrogate pair as Unicode. |
| Isohybrid detection as ISO 9660 | Xorriso's synthetic hybrid has an ISO hierarchy plus MBR and GPT structures, reported by `-report_system_area plain`. |

Owner decision 8 requires strict primary order; Joliet order is not checked
because of H3's Joliet inversions. The remaining oracle work is tracked in
[ISO 9660 work](todo-iso9660.md).

The small raw-byte reader reproduces directory and L/M path-table inversion
counts, PX lengths, SP skip values and relocation placeholder extents:

```sh
ruby test/iso9660-evidence.rb "$HOME/.cache/fsimg-iso/corpus"
ruby test/iso9660-evidence.rb "$HOME/PCM-3.1-v4.76-disc-image.iso"
```

It emits only counts and numeric metadata, reads no file payloads, and is
distributed in `EXTRA_DIST`; its CLI is not a `make check` test. It handles the
corpus's 2048-byte blocks and SP-skip-zero records; it is an evidence reader,
not a general verifier. Other raw-byte measurements above, including dates,
sequence numbers and continuation boundaries, come from manual inspection.

The focused generator checks run with `ruby test/iso9660-corpus-test.rb`
and through the configured `test/iso9660-corpus-check.sh` in `make check`.
They cover sparse output, producer diagnostics and failure, extraction byte
counts, listing normalization and source-manifest semantics. The corpus
generator and documentation do not change the library or CLI.

Validation uses `autoreconf -fi`, an out-of-tree configure and
`make -j32 check`, followed by `make distdir` to check inclusion of the
corpus source files and the evidence reader.

### ISO 9660 reader checks

`test-iso9660` builds every acceptance fixture in memory with its own
encoder, which shares no code with the driver, and runs in the plain
suite. `test/iso9660-integration.sh` compares the primary, Rock Ridge and
Joliet views with a corpus and reads the owner's disc; it skips with
status 77 unless given them:

```sh
FSIMG_ISO_CORPUS="$HOME/.cache/fsimg-iso/corpus" \
FSIMG_ISO_H3="$HOME/PCM-3.1-v4.76-disc-image.iso" \
sh test/iso9660-integration.sh
```

For every corpus image, `fsimg t` and `fsimg x` with `-o view=primary`
agree with the `*.primary.jsonl` manifest: paths with the final `;1`
removed, kinds, sizes and the SHA-256 of every file. For every image
carrying Rock Ridge, `fsimg i` must report the `rockridge` profile under
`view=auto`, and `fsimg t -v` must agree with `isoinfo -R -l` and
`bsdtar -tv` on paths, kinds, owner, mode, modification times to each
tool's printed precision and link targets, and with the
`*.rockridge.jsonl` manifest on content. A whole-tree `fsimg x -k` in the
Rock Ridge view must create exactly the manifest's directories and files,
with their SHA-256, and report its symbolic links and FIFOs as skipped,
since safe extraction creates neither. bsdtar is skipped only for
`xorriso-deep.iso`, whose relocation libarchive cannot connect. The
script then needs `bsdtar` as well. The isohybrid image is read without
forcing, and `W` must print one note naming its partition table. Against the stage 0 corpus all 36 images agree, 31 of
them in the Rock Ridge view. Forced Rock Ridge must match automatic
selection. Joliet names, kinds, sizes and file digests agree with its
manifest and the independent listings, with the documented xorriso
parent-record refusals. The H3 pipeline discards diagnostics and filters
listings directly to counts: 7,763 files and 2,204 directories including
the root, in automatic and forced Joliet views, compared with
`isoinfo -J -l`. Information confirms automatic Joliet selection; `W`
succeeds under automatic and primary selection. No H3 member name or
volume label enters a transcript or temporary file.

For every corpus image, JSON info's boot catalog block agrees with live
`isoinfo -d` and the saved descriptor report; boot entry, BIOS and EFI
counts agree with `xorriso -report_el_torito plain`. All 36 stage 0 corpus
images agree, including the nine bootable images; non-bootable images
report four zero boot details.

`make check-scale` runs `test-iso9660-scale` with `--wide`, `--deep`,
`--depth-limit`, `--sections` and `--capacity`, each in a separate process.
Its callback generates sectors on demand without an image file or an
in-memory catalog. Only private, disposable metadata scratch reaches the
host filesystem. Each case uses a 16 MiB resource memory budget and checks
memory, scratch and descriptor accounting after freeing the reader and I/O.
The million-entry case walks and verifies one directory of empty files
and spills directory metadata to scratch. The separate million-data-claim
acceptance fixture exercises the verifier's claim-sort spill. The Rock
Ridge cases walk 256 directory
levels below the root and assert a depth-limit error on level 257. The
4 TiB file has 1,025 Level 3 sections separated by unreferenced blocks;
reads on both sides of every boundary check the physical callback offset.
Its verification checks metadata and ownership without reading 4 TiB of
zero payload. The capacity case reads a file in the final block and
verifies a small tree in a volume of
4,294,967,295 logical blocks (8,796,093,020,160 bytes).

Reference macOS native measurements, in bytes except calls and passes:

| Case | Source read calls | All I/O calls | Metadata passes | Peak scratch | Peak charged memory | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Million-entry directory, walk and W | 95,280 | 278,956 | 4.000 | 132,000,643 | 5,364,225 | 12,378,112 |
| Rock Ridge, 256 levels | 276 | 276 | 1.000 | 0 | 1,145,097 | 2,297,856 |
| Rock Ridge, level 257 refused | 276 | 276 | 0.996 | 0 | 1,145,097 | 2,297,856 |
| 4 TiB file, boundary reads and W | 2,189 | 2,189 | 4.000 | 0 | 1,200,501 | 2,400,256 |
| Maximum block count, content and W | 43 | 43 | 2.000 | 0 | 1,150,193 | 2,363,392 |

Metadata passes are callback directory-sector deliveries divided by the
number of directory sectors; the refused walk is a partial pass. All I/O
calls include scratch transfers, whereas source read calls count only the
image callback. RSS is `getrusage(RUSAGE_SELF).ru_maxrss`, converted to
bytes on platforms reporting KiB; separate processes avoid inherited peaks
from other profiles. Measurements cover macOS; Linux and FreeBSD scale
comparisons are not available here.

`test-iso9660-faults` supplements the acceptance tests with stage-separated
allocation, scratch open/resize/read/write, positive short read, EOF and
cancellation sweeps for Rock Ridge, Joliet and bootable images. It checks
open, walk, file content and payload verification; every injected attempt
releases memory, scratch and descriptors. A zero checkpoint count means
that operation needs no allocation or scratch I/O. The independent ISO
profile in `test-driver-contracts` exercises the common reader contracts.

`fuzz/iso9660.c` runs under `check-fuzz` with primary and BIOS-plus-EFI
seeds that `test-iso9660 --seed` and `--seed-boot` write at run time.
`fuzz/iso9660-susp.c` keeps a fixed synthetic block map and fills its
system use areas and continuation blocks from the input, so mutation
concentrates on SUSP and Rock Ridge decoding; its seed comes from
`test-iso9660 --susp-seed`.

Reader qualification runs `sh ../build-aux/ci-check.sh` from the configured
build directory. On macOS, `FSIMG_ISO_CORPUS` names the generated corpus
and `FSIMG_ISO_H3` names the owner's read-only disc, so the ordinary,
sanitizer and distribution suites include both integrations. FreeBSD uses
`MAKE=gmake MAKEFLAGS=-j32 sh ../build-aux/ci-check.sh`, with the locked
specification gems installed and the fatcat oracle built as described above.
Linux runs `build-aux/linux-check.sh -r REV 'sh ../build-aux/ci-check.sh'`
from the source root. A separate Linux build runs `../configure CC=gcc
CXX=g++` and `make -j8 check`, including the CLI and tests under GCC's
C11 `-O2 -Wpedantic -Werror` diagnostics.

| Platform | Reader qualification |
| --- | --- |
| macOS | Full CI list passes; ordinary, ASan/UBSan, separate UBSan and distribution suites each report 65 passes and six environment skips. Corpus and H3 integration pass. |
| Linux | Full CI list passes, including standalone LSan and MSan; ordinary, sanitizer and distribution suites each report 68 passes and three environment skips. The separate GCC suite also passes with those counts. |
| FreeBSD 16 | Ordinary, ASan/UBSan, separate UBSan and distribution suites each report 65 passes and six environment skips; the specification suite and Capsicum test pass. All five ISO scale profiles pass. |

The external ISO corpus and H3 are absent from the Linux and FreeBSD
runs, where `iso9660-integration.sh` skips. The MSan build disables the
optional xz and zstd profiles as described in the sanitizer instructions.

Standalone LSan and MSan report unsupported-platform skips on macOS and
FreeBSD.

On macOS, `FUZZ_CC=/usr/local/opt/llvm/bin/clang
FUZZ_CXX=/usr/local/opt/llvm/bin/clang++ make -j32 check-fuzz` passes all
16 harnesses at 1,000 runs each. A separate ASan/UBSan ISO verifier run
uses `-fork=4 -max_total_time=600 -max_len=1048576 -timeout=10` with
primary, boot, Joliet, Rock Ridge and relocated-tree seeds. It completes
308,995 inputs in 612 seconds with no crashes, timeouts or out-of-memory
failures.

The ISO probe's 34,816-byte prefix raises the registry's detection
prefix from 8,704 bytes (QNX6) to 34,816 bytes; a volume shorter than the
prefix is read whole. The measurement ran `fsimg i -f IMAGE` without a
forced format, fifteen times per image, under `/usr/bin/time -l`, with the
build of the preceding revision and with the ISO reader, on a MacPro7,1
(Xeon W-3245, macOS 26.6.2). Each fixture is a small image `fsimg c`
created; the FAT12 (18,432 bytes), QNX4 (22,528 bytes) and ImageFS (176
bytes) fixtures are shorter than the new prefix. Wall times include
process start and the worker:

| Format | Median wall before (ms) | After (ms) | Peak RSS before (bytes) | After (bytes) |
| --- | ---: | ---: | ---: | ---: |
| FAT12 | 40.7 | 27.9 | 1,392,640 | 1,363,968 |
| FAT32 | 23.2 | 24.2 | 1,331,200 | 1,478,656 |
| exFAT | 22.6 | 24.8 | 2,568,192 | 2,674,688 |
| QNX6 | 22.3 | 24.5 | 2,433,024 | 2,523,136 |
| QNX4 | 22.4 | 24.5 | 2,408,448 | 2,437,120 |
| ImageFS | 22.4 | 24.5 | 2,420,736 | 2,445,312 |

Detection costs about 2 ms and at most about 150 KB of peak RSS more per
open on these fixtures, within process-start noise. The comparison
baseline records the larger prefix as extra bytes read by the small exFAT
fixtures.

### ISO 9660 writer checks

`test-iso9660-write` reaches the writer through the public constructor and
runs in the plain suite. It checks every
image twice: with an independent checker written from the format design,
which decodes the descriptors, both hierarchies, their path tables, every
Rock Ridge entry through its continuation chain and the file data and
requires every other byte to be zero over a poisoned sink; and with
fsimg's reader in the automatic, Rock Ridge, Joliet and primary views and
`W` with payload. It also pins identifiers and aliases, record and
continuation boundaries, files above 4 GiB through a sparse sink,
refusals, time losses, emission faults, and allocation, scratch and
cancellation failures at every point.

The sparse large-file fixture also checks each hierarchy's multi-extent
directory-record run independently of the reader: both byte orders, section
lengths and flags, identifiers, dates and contiguous extents. This check
does not materialize or hash the multi-gigabyte payload. Additional boundary
fixtures cover path tables crossing blocks, 65,535 total directories and
refusal of one more, and BMP labels with multibyte UTF-8 characters. Corrupt
scratch rows exercise oversized file sizes and continuation positions that
would wrap during block alignment; both fail with `FSIMG_ECORRUPT`.

`./test-iso9660-write --create FILE LEVEL [short]` writes its test tree,
which has every entry kind, aliased and non-BMP names, a 255-byte name,
1023-byte link targets and an eight-level tree, for the external readers;
`short` leaves out names and targets longer than 200 bytes. On Ubuntu
24.04 with xorriso 1.5.6, genisoimage's `isoinfo` and `isovfy` 1.1.11,
bsdtar 3.7.2 and p7zip 16.02, at levels 1, 2 and 3:

* `xorriso -indev FILE -find /` loads the tree without a message;
* `isovfy` finds no errors in the `short` images; it overflows a fixed
  buffer on 255-byte names and long targets, and misparses
  genisoimage's own long `SL` chains;
* `isoinfo -d` reports Rock Ridge 1 and Joliet level 3, and `-f`, `-R -l`
  and `-J -f` list the tree; `isoinfo` itself prints uid 1000 as 1750,
  as it does for genisoimage images;
* `bsdtar -x` in the Rock Ridge view extracts every file's bytes, owner,
  mode and time, and every link target held in one `SL` entry, exactly,
  and in the Joliet view (`--options iso9660:!rockridge`) the Joliet
  names; `7z x` extracts the
  Joliet tree with the same file contents.

Two consumer limits shaped the writer. libisofs refuses on reading a link
target over 1023 bytes or with a component over 255 bytes, so the writer
stores at most that. libarchive finds Rock Ridge only from an entry it
decodes in the root's first dot record, so that record keeps `SP`, `PX`
and `TF` and only `ER` continues, as mkisofs and xorriso write it.
libarchive also joins the first component of every `SL` entry after the
first without its `/`, so a target spanning several `SL` entries
extracts wrong from fsimg, genisoimage and xorriso images alike; the
comparison above excepts those targets.

The complete native gate is `MAKEFLAGS=-j32 sh build-aux/ci-check.sh`,
run from the configured build directory (`MAKE=gmake` on FreeBSD). It
includes the plain suite, ASan/UBSan, separate UBSan, install, scale, CLI
RSS and distribution checks. Standalone LSan and MSan run on Linux and
report unsupported on macOS and FreeBSD. Kernel mounts are separate
qualification runs comparing each view's complete manifest.

### ISO writer consumer qualification

`test/iso9660-writer.sh` runs without a private corpus or H3 image. Its
synthetic writer fixtures cover levels 1, 2 and 3, names and metadata,
relocation, case collisions, and an ordinary populated `rr_moved`
directory created through the CLI. The C fixture checker independently
compares emitted bytes and metadata with the source specifications. The
Ruby adapter then compares consumer listings and extracted bytes with
complete fsimg manifests. File hashes stream in bounded chunks.
`ISOINFO`, `ISOVFY`, `XORRISO`, `BSDTAR` and `SEVENZIP` select configure-time
oracles. Missing consumers and inapplicable cases are reported explicitly;
a fixture with no applicable consumer is skipped, never reported passed.

Xorriso reads the medium, compares its Rock Ridge names, numeric metadata,
observable modification times and link targets, then streams every regular
file for an exact byte count and SHA-256 comparison. A successful media
load alone is insufficient: a fake-consumer regression reports a wrong
name with successful status and requires the comparison to fail. Isoinfo compares every namespace's paths,
kinds and regular-file sizes. Isovfy checks the short
fixture because its fixed buffers cannot hold the full fixture's long
names and link targets. Libarchive checks numeric metadata and observable
modification times, and extracts each regular file for a byte count and
SHA-256 comparison. 7-Zip checks the Joliet listing and contents. On a
case-sensitive host filesystem the adapters also extract complete trees.
On a case-insensitive filesystem they report that whole-tree comparison
unsupported and use listings plus per-file streaming extraction, which
cannot overwrite a case-distinct sibling.

Consumer display conventions are separate from the core name rules:

* macOS libarchive emits decomposed Joliet names. Its adapter compares
  canonical Unicode equivalence under NFC, with independent collision
  checks on both expected and observed namespaces and unchanged content
  hashes. This is not byte-identical path spelling.
* Libarchive omits the final primary-file dot for an empty extension. Its
  adapter removes that dot from files only. A source namespace containing
  a file `FOO.` and directory `FOO` is not injective under that projection;
  that consumer/view is reported unsupported, not silently collapsed.
* Libarchive 3.7.4 and libisofs 1.5.8 in xorriso 1.5.8.pl02 lose four
  separators in the full fixture's 1,023-byte `/long-target` across `SL`
  entries, yielding the same exact 1,019-byte target. Only that measured
  target and resulting byte count are accepted as a consumer exception.
  Unrelated target corruption still fails; kernel readers and fsimg
  compare the original target exactly. Xorriso's own producer/reader also
  loses six separators for a 1,023-byte target of 512 one-byte components,
  proving the reader limitation independently of fsimg's encoder. Its
  producer splits long text components to fill each `SL` entry, which
  avoids the affected boundary in its three-255-byte-component fixture;
  this packing choice does not fix the many-small-component case.
* Xorriso 1.5.6 loses the doubled interior separator of `/odd`, changing
  `a//b/./../c/` to `a/b/./../c/` in both listing and extraction. Only this
  exact source/result pair is a consumer exception; its other seven link
  targets and all regular-file hashes match. Its own producer also loses a
  separator on a round trip. Fsimg and both kernels require exact targets.
* Libarchive suppresses the root `rr_moved` or `.rr_moved` directory entry,
  including directory metadata, even when ordinary descendants exist.
  The known fixture exception covers only that one directory entry; all
  descendants and file hashes remain mandatory. Other oracles and kernel
  readers require the complete directory entry and its metadata.
  When extraction synthesizes that parent for its children, the adapter
  checks its kind and size while omitting only metadata whose directory
  header was demonstrably absent from the listing. These observations agree
  with the reader's relocation-name heuristic in
  [libarchive's ISO reader](https://github.com/libarchive/libarchive/blob/master/libarchive/archive_read_support_format_iso9660.c);
  no implementation code is imported.

The command environment uses `C.UTF-8` and UTC for both metadata listings
and streamed payload reads. Captured text is decoded explicitly; payload
bytes are hashed without transcoding. The required cdrtools observer uses
explicit UTF-8 output for Joliet names, retaining the complete Unicode
qualification profile. The listing parser
learns fixed padding from each directory's dot record, preserving actual
filename whitespace.

The populated-directory fixture uses a 128 KiB volume so optical oracles
can read a complete transfer window. Isoinfo 3.02a09 reports a short read
on a valid smaller image; fsimg's minimum-volume tests remain independent
of this consumer limitation.

`FSIMG_ISO_LARGE=1 sh test/iso9660-writer.sh` adds a real sparse image
containing an 8 GiB multi-extent file and a `UINT32_MAX`-byte single-section
file. Large payload comparisons use streaming extraction instead of
materializing large extracted files. The regular fixture's metadata tests
and the independent sparse-section checker still run in the ordinary
suite. Isoinfo 3.02a09 qualifies the multi-extent listing but its extraction
returns only a section, so it does not qualify large-file payloads.

`test/iso9660-kernel.rb IMAGE MANIFEST` is an explicit privileged
qualification command, outside ordinary tests. Create the manifest with
`ruby test/iso9660-writer.rb --manifest IMAGE BUILD-DIRECTORY`. The kernel
runner mounts only a regular, non-symlink fixture file, read-only, beneath
its own temporary root. Linux uses a disposable privileged container;
FreeBSD uses a private vnode-backed md device. Cleanup is registered before
the mount attempt and reports primary and cleanup failures together;
failed unmounts never trigger recursive deletion through a retained mount.
Fake-command regressions pin both failure paths.

Each kernel view compares the entire `lstat` namespace, file bytes, link
targets, owners, modes and every stored time the kernel exposes. Linux
Joliet uses `norock,utf8,check=strict`; its primary view uses
`norock,nojoliet,map=off`, preserving the ISO spelling before the common
display rule removes `;1`. FreeBSD selects views with `-r`, `-j`, `-g` and
`-C UTF-8`. Native FreeBSD cd9660 exposes multi-extent sections as individual
directory records with a 32-bit section size. The large-file kernel case
therefore reports `UNSUPPORTED` and exits 77; that is not a passing check.
Fsimg, Linux and capable userspace readers compare all large-file bytes.

### ISO writer scale checks

`make check-scale` includes `test-iso9660-write-scale --wide` and
`--capacity`. Both use the public writer with a 16 MiB context. The wide
fixture generates one million empty files directly in the catalog,
publishes the image, releases the writer, then fully verifies every
offered view under the same aggregate budget. It records catalog admission
plus planning time and I/O separately from emission and verification,
charged memory and scratch peaks, descriptor peak and process RSS. The
capacity fixture plans a one-file volume of `UINT32_MAX` blocks without
creating an 8 TiB host file. Both check complete context resource release.

The macOS wide run reports 37.517 seconds for catalog admission and
planning, 3.228 seconds for emission, and 14.410 seconds for verification;
the image is 182,583,296 bytes. It records 2,030,773 admission/planning I/O
calls and 414,828 emission calls, 5,427,808 peak charged writer bytes,
649,001,240 peak logical scratch bytes, 11 descriptors and 24,551,424
process RSS bytes. The capacity plan reports 8,796,093,020,160 volume bytes,
1,262,224 peak charged bytes and 2,367,488 RSS bytes. These are plain-build
measurements; sanitizer runs are separate.

### ISO 9660 firmware checks

`test/iso9660-boot.sh` assembles the project-owned BIOS image with NASM,
cross-compiles the project-owned EFI application with clang and lld-link,
and creates its FAT12 EFI system image with fsimg. Both source fixtures
are BSD-2-Clause under `test/fixtures/iso9660/`; no firmware application
binary is imported or distributed. The test helper's `--create-boot`
operation creates BIOS-only, EFI-only and combined images through the
registered writer. Each boot image remains an ordinary file with its
original bytes. The structural C tests check catalog checksums, exact file
extents and payloads, every view's verification, setting refusals, BIOS
load bounds and EFI sector-count boundaries independently.

The firmware check compares the catalogs with xorriso, then boots each
applicable image in QEMU with SeaBIOS and OVMF. Each application reports
a serial marker and exits through the test VM's debug port. VMs use TCG,
have no network interface or host disk, and are killed after 20 seconds.
Each OVMF run gets a disposable copy of the variable store. The check
requires all three firmware paths and skips explicitly when they or its
development tools are absent. For the Homebrew QEMU and lld packages:

```sh
LLD_LINK=/usr/local/opt/lld/bin/lld-link \
FSIMG_SEABIOS=/usr/local/share/qemu/bios.bin \
FSIMG_OVMF_CODE=/usr/local/share/qemu/edk2-x86_64-code.fd \
FSIMG_OVMF_VARS=/usr/local/share/qemu/edk2-i386-vars.fd \
    sh test/iso9660-boot.sh
```

The macOS firmware run passes BIOS-only, EFI-only and combined images with
QEMU 11.1.2 and SeaBIOS 1.17.0. SeaBIOS executes a catalog with a flipped
checksum, so that mutation is a negative fsimg verification test. The
firmware refusal test damages the validation key and confirms the BIOS
marker is absent through the timeout. Firmware execution is evidence that
the image boots, not that firmware validates its catalog's integrity.

### ISO 9660 relocation checks

`test-iso9660-write` exercises relocation by physical depth and path length,
including logical depths 7, 8, 9, 16 and 256, several relocations under one
parent and relocation inside moved subtrees. The independent checker walks
the primary physical hierarchy and reconciles each CL placeholder with one
RE record and its directory's PL parent, while checking both path tables,
link counts, continuation ownership and every padding byte. A long-name
fixture makes the logical record continue while the physical record fits,
so the two record plans cannot accidentally share their placement.

Additional cases adopt an empty source `.rr_moved`, refuse a nonempty or
non-directory conflict only when relocation is needed, compare images
across source order, cache size and scratch choice, and extract the logical
tree before scanning and creating it again. The allocation, scratch and
cancellation sweeps run on both ordinary and relocated trees. Source-read
errors preserve their status; a target that becomes unrepresentable after
planning is instead a source change.

`test-iso9660-write --create-relocated FILE LEVEL` supplies a synthetic
relocated tree to independent consumers. `--create-large FILE` writes a
real sparse image with a multi-extent file and an exact 32-bit-size file,
without a payload-sized memory sink. Its output parent must be private;
large-image qualification hashes files by streaming rather than extraction
to an allocated multi-gigabyte host file.

### Declared geometry and selected input checks

`test-protocol` accepts the full 64-bit descriptor-declared geometry range
in an informational record. `test-worker --info-bounds` sends maximal
geometry from a forged worker and then tries to stream more bytes than the
selected input permits. Default, explicit-lower-limit and nonzero
offset/length cases must all fail at the independent output bound without
forwarding the oversized batch.

`test-iso9660 --output-limit` extracts 44 files sharing an accessible
96 KiB extent from a 256 KiB selected view whose descriptor declares 2 MiB.
The default refuses cumulative output beyond 16 times the selected view, while
explicit context and extraction limits retain their precedence. A nonzero
slice prefix also verifies that the budget uses the selected view rather
than its larger backing object. The CLI shared-extent case exercises the
same directory-writing path with default and explicit limits.

`test/cli/iso_truncated_test.rb` derives metadata and payload positions from
the independent ISO seed. An unused missing tail still permits information,
listing and content delivery; missing file blocks instead produce the
filesystem's corruption status for listing and extraction. The focused
commands in a configured build directory are:

```sh
make -j32 fsimg test-protocol test-worker test-iso9660
./test-protocol
./test-worker --info-bounds
./test-iso9660 --output-limit
ruby ../test/cli/run.rb ./fsimg --name '/test_iso_(unused_missing|missing_payload)/'
```
