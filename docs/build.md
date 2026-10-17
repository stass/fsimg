# Building and validating fsimg

Development requires Autoconf, Automake, Libtool, make, a C11 compiler, and a
C++ compiler. On macOS install missing tools with `brew install autoconf
automake libtool`. Independent FAT tests additionally use `brew install mtools
dosfstools`. Linux and FreeBSD use their native development packages.
Release archives include generated Autotools support and build without those
generators. The installed library has no third-party runtime dependencies.

From a checkout:

```sh
autoreconf -fi
mkdir build
cd build
../configure
make
make check
make check-security
make check-integration
make check-asan
make check-ubsan
make check-lsan
make check-msan
make check-scale
make check-fuzz
```

`check` builds and runs native tests, C11/C++ header examples, and an ASCII
source/build check. Test logs are in `test-suite.log` and individual `.log`
files. Fixtures are relative to the disposable build directory and cleaned on
success. `check-security` runs the publication and sandbox harnesses directly;
a sandbox skip is printed explicitly. Automake records exit 77 as SKIP.
The sandbox harness forks disposable single-threaded children, closes inherited
handles, disables core dumps, installs the profile, and attempts real forbidden
operations. It must run on the kernel being qualified.

Sanitizer targets configure separate build directories beneath the current build
and run the full suite. `check-asan` combines ASan and UBSan, enables stack
use-after-return and scope checks, and enables leak and initialization-order
checks on Linux. macOS ASan does not support leak or initialization-order
checks. ASan/fuzz runs halt on UBSan findings and print a stack trace. Standalone
LSan/MSan targets print SKIP on non-Linux hosts; this is not a passing check.
Linux MSan requires Clang and an appropriate runtime; an unsupported compiler
or runtime fails configuration rather than being reported as a pass. TSan is
not applicable to the serial library and process-only test harnesses.

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

`check-integration` creates images with dosfstools and populates them with mtools.
The ordinary suite includes this script and explicitly skips it if these
development tools are absent. The dedicated target returns that skip rather
than claiming success. The manifest is `test/fat-integration.sh.log`: formatter
and validator versions, geometry, population commands, a forced fragmentation
recipe, and outcomes. Fixtures include all FAT variants and a 4096-byte-sector,
8192-byte-cluster FAT16 case. Bytes and Unicode names are checked, then complete
payload verification runs. No test mounts an image or needs root.

`check-fuzz` builds and checks a separate `sanitizer-fuzz` static library with
ASan/UBSan and `-fsanitize=fuzzer-no-link` coverage on every object, then compiles
`fuzz/fat.c` with Clang's libFuzzer runtime, and creates seeds for all FAT variants
using the independent fixture builder. It runs 1000 mutations with a 3 MiB input ceiling
and a ten-second per-input timeout. Each reader has a 16 MiB budget, 1000-entry,
eight-level and 250,000-work limits, and no scratch authority. Each input is
read both as an exact-length byte array and as a prefix of zero-filled logical
storage. The latter reaches FAT32's minimum geometry without a large resident
seed; it does not allocate or transfer the logical volume. The target fuzzes
probe, directory/name parsing, chain traversal, content/range reads, index
construction, and full verification; it also checks cleanup accounting.
Set `FUZZ_CC` to a Clang with the required runtime; `FUZZ_CXX` defaults to its
sibling `clang++`. Use a fresh build directory when changing compilers. Apple
Clang lacks the libFuzzer runtime; macOS fuzz validation uses Homebrew LLVM:

```sh
brew install llvm
FUZZ_CC="$(brew --prefix llvm)/bin/clang" make check-fuzz
```

The exact standalone commands from a configured build are:

```sh
make test-fat
sh ../build-aux/fuzz.sh "$(cd .. && pwd)"
```

The target's `fuzz-corpus/` and libFuzzer crash artifacts are disposable build
outputs. A finite smoke run is not a sustained fuzzing campaign.

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

The extended ten-million-file catalog uses the same generated provider, 10,000
explicit directories, a 256 MiB budget, and no retained per-file object or FD:

```sh
./test-catalog --scale 10000000
```

Native macOS validation reports thirteen passing tests and the explicit sandbox
skip in ordinary, ASan/UBSan, and standalone UBSan runs. The metadata scale
fixture reports a 329,088-byte allocation peak, 75,795,888-byte logical scratch
peak, and 134,683 I/O calls on APFS. These are measurements of this fixture,
not bounds promised for every metadata workload. Linux/FreeBSD checks require
their own host runs.

The instrumented fuzz smoke run uses Homebrew LLVM 23.1.2 and completes 1000
mutations with FAT12/16/32 seeds, reporting `cov: 2072` and `ft: 5734`
without sanitizer or cleanup-accounting findings. These counters describe one
finite run and vary with the generated mutation sequence.

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

Stage reviews include the required independent code-review agent and an external
read-only OpenCode review through the Z.ai coding-plan subscription. Copy only
project sources, tests, and contracts into a review directory, excluding Git
metadata, configuration, and credentials, and provide a `REVIEW.md` prompt
covering correctness, design, security/information leaks, and the style guide's
readability review. Run from that directory:

```sh
OPENCODE_CONFIG_CONTENT='{"share":"disabled","permission":{"*":"deny","read":"allow","glob":"allow","grep":"allow"}}' \
  opencode run --pure --agent plan --model zai-coding-plan/glm-5.3 \
  --variant max --format json 'Read REVIEW.md and perform the requested review.'
```

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
make install DESTDIR="$PWD/install-root"
make uninstall DESTDIR="$PWD/install-root"
make distcheck
```

`include/fsimg.h` documents implemented entry points and ownership. An options
or callback table must be initialized and have exactly the supported size and
version. The ABI is experimental. Library statistics include allocation headers
and the context itself, and exclude caller buffers, allocator bookkeeping,
thread stacks, mappings, and kernel caches. Cumulative I/O statistics saturate
at `UINT64_MAX`. Memory and descriptor quotas aggregate across live handles;
private session counters persist across incremental calls. An output ceiling
of zero is derived by the containing reader/writer from its volume length; optional
work ceilings are disabled at zero.

The installed header/library expose FAT readers, volume information, current-file
content/range reads, full verification, and independent reader/writer capability
enumeration. No command-line executable is installed. Public FAT writers expose
collection, immutable planning, raw emission, and durable no-replace publication.
Source access and common catalog/storage remain private. The sandbox remains a
private mechanism prototype without worker supervision; image staging and
publication have a shared library coordinator.
