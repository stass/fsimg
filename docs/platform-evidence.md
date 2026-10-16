# Platform mechanism evidence

The native harnesses are `test-sandbox` and `test-publish`. Their logs, host
kernel, compiler, and filesystem identify evidence; compiled platform branches
alone do not establish enforcement. CI runs ordinary and security tests on
Linux, FreeBSD, and macOS. Jobs that require confinement reject a SKIP.
Linux and FreeBSD CI jobs require execution before their profiles are qualified;
no remote kernel run is recorded here.

| Mechanism | Scope exercised | Host evidence |
| --- | --- | --- |
| macOS confinement | Explicit unavailable result before restriction | Native test returns SKIP; no standalone backend claimed |
| Linux confinement | Landlock ABI >= 3; x86_64 and aarch64 seccomp architecture checks | Requires Linux kernel execution |
| FreeBSD confinement | Capsicum descriptor, ioctl and fcntl rights | Requires FreeBSD kernel execution |
| Regular-file publication | Exclusive hardlink, staged unlink, both directory syncs | Native publication tests; qualify each deployment filesystem separately |

Local checks use Darwin 25.6.0 on x86_64, Apple Clang 21.0.0, and APFS. Native
`make check`, combined ASan/UBSan, and standalone UBSan each report seven passing
executables and one sandbox skip, with no sanitizer findings. `make distcheck`
passes archive rebuilding, tests, installation, uninstallation, and cleanup.
The shared library links only to the host's libSystem. Publication tests cover
APFS; this does not qualify other filesystems. Standalone LSan and MSan report
unsupported-host skips. Linux/FreeBSD confinement and their sanitizer runs
remain unverified locally.

The metadata `check-scale` fixture sorts 24 MiB and fills/queries a 24 MiB array
at a 16 MiB context budget. It reports 329,072 bytes of charged peak memory,
75,795,888 logical scratch bytes, 75,776,000 sampled allocated scratch bytes,
134,683 I/O calls, 259,889,616 read bytes, and 178,042,928 written bytes. The
observed elapsed time is 1.433 seconds and process `ru_maxrss` is 1,228,800 bytes.
The cache is 16 pages, the run allowance 256 KiB, and merge fan-in eight.
This single-process metadata measurement does not establish the CLI/worker RSS
acceptance gate or gigantic-volume/catalog qualification.

Linux requires kernel headers containing Landlock truncation protection; an
older build environment reports the profile unavailable. Runtime ENOSYS or
EOPNOTSUPP from the initial ABI query is unavailable, an older ABI is
unavailable, and every unexpected or later setup error is an enforcement
failure. Linux uses no libseccomp. The allowlist permits positional I/O,
allocation, signals, time queries, private directory operations, `F_GETFL`,
and Landlock-mediated `openat` without `O_PATH`. It denies pathname stat,
metadata mutation, network, execution, descriptor duplication/acquisition,
process creation, ioctl, and io_uring. The x32 ABI is killed. These decisions
follow the kernel [Landlock](https://docs.kernel.org/userspace-api/landlock.html)
and [seccomp](https://docs.kernel.org/userspace-api/seccomp_filter.html)
contracts. Actual libc syscall compatibility remains part of kernel testing.

Worker descriptor cleanup requires Linux `close_range` or FreeBSD `closefrom`.
Failure to establish complete closure prevents worker startup; resource limits
cannot bound a fallback loop because descriptors survive a lowered limit. A
regression retains a high descriptor, lowers the limit, and checks that cleanup
either closes that descriptor or refuses to claim success.

FreeBSD limits input to reading/seek/stat and flag inspection, output roots to
creation/read/write/seek/stat/sync/truncation/removal, and optional source roots
to lookup/read/seek/stat. No ioctl or metadata-mutation right is granted.
The prototype requires no inherited sockets or executable descriptors. Capsicum
blocks ambient namespace access, as specified by
[cap_enter(2)](https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD+14.3-RELEASE+and+Ports&query=cap_enter&sektion=2).
A source-root profile does not claim to prevent opening preexisting devices.

The publication prototype operates on a completed regular file in private
staging after its writer has exited. It validates type and expected size,
flushes and closes it, links the final name without replacing anything, removes
the staged link, and syncs both directories. A competing existing file,
directory, or symlink is never replaced. Unsupported linking is explicit;
there is no ordinary-rename fallback. A failed link with an ambiguous error
reports uncertain publication. Any cleanup/sync failure after linking reports
published output with uncertain durability and must not be retried blindly.
Directory-sync support is filesystem-specific and an unsupported/error result
is not success. This prototype does not claim crash testing or remote-filesystem
qualification.
