# Building and validating fsimg

Development requires Autoconf, Automake, Libtool, make, a C11 compiler, and a
C++ compiler. On macOS install missing tools with `brew install autoconf
automake libtool`. Linux and FreeBSD use their native development packages.
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
make check-asan
make check-ubsan
make check-lsan
make check-msan
make check-scale
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
checks on Linux. macOS ASan does not support those latter checks. Standalone
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

`check-scale` is a separate, non-sanitized metadata qualification: it sorts 24 MiB
of generated records and fills/queries a 24 MiB fixed-record array with a 16 MiB
context budget, a 16-page shared cache, a 256 KiB run allowance, and merge fan-in
eight. It prints charged memory, logical and sampled physical scratch peaks,
I/O counts/bytes, elapsed time, and the host's raw `getrusage` maximum RSS value
(bytes on macOS, KiB on Linux/FreeBSD). Generated data and fixed-record lookups
have deterministic expected results without a resident scale oracle. This
qualifies metadata spilling separately from future format/catalog scale gates.
Scratch should use a disk-backed filesystem; a RAM-backed directory consumes
system memory outside the library heap budget.

Native macOS validation reports seven passing tests and the explicit sandbox
skip in ordinary, ASan/UBSan, and standalone UBSan runs. The metadata scale
fixture reports a 329,072-byte allocation peak, 75,795,888-byte logical scratch
peak, and 134,683 I/O calls on APFS. These are measurements of this fixture,
not bounds promised for every metadata workload. Linux/FreeBSD checks require
their own host runs.

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

No command-line executable or format capability is installed by this substrate
build. The sandbox and publication functions are private mechanism prototypes;
they do not provide worker supervision, staging creation, or filesystem parsing.
