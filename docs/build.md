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
