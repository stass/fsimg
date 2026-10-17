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
| Bounded host scanning | Native directory batches, no-follow component opens, mount identity, source-change checks | Ordinary native macOS source tests; actual descendant-mount fixtures and Linux/FreeBSD runs remain open |

Local checks use Darwin 25.6.0 on x86_64, Apple Clang 21.0.0, and APFS. Native
`make check`, combined ASan/UBSan, and standalone UBSan each report ten passing
executables and one sandbox skip, with no sanitizer findings. `make distcheck`
passes archive rebuilding, tests, installation, uninstallation, and cleanup.
The shared library links only to the host's libSystem. Publication tests cover
APFS; this does not qualify other filesystems. Standalone LSan and MSan report
unsupported-host skips. Linux/FreeBSD confinement and their sanitizer runs
remain unverified locally.

The metadata `check-scale` fixture sorts 24 MiB and fills/queries a 24 MiB array
at a 16 MiB context budget. It reports 329,088 bytes of charged peak memory,
75,795,888 logical scratch bytes, 75,776,000 sampled allocated scratch bytes,
134,683 I/O calls, 259,889,616 read bytes, and 178,042,928 written bytes. The
observed elapsed time is 1.826 seconds and process `ru_maxrss` is 1,355,776 bytes.
The cache is 16 pages, the run allowance 256 KiB, and merge fan-in eight.
This single-process metadata measurement does not establish the CLI/worker RSS
acceptance gate or gigantic-volume qualification.

Catalog scale checks use generated provider content. A 100,000-file root at a
16 MiB budget reports 3,157,912 charged memory bytes, 58,500,368 logical scratch
bytes, seven owned descriptors, and 86,607 I/O calls in 8.990 seconds. Grouping
100,000 files beneath 100 directories at a 256 MiB budget reports 5,255,064 memory
bytes, 61,558,724 scratch bytes, seven descriptors, and 75,894 I/O calls in 8.179
seconds. Each generated path, source ID, and parent is checked without retaining
a full oracle. These figures measure library accounting separately from RSS.

The extended catalog contains 10,000,000 files and 10,000 explicit directories
at a 256 MiB budget. It reports 5,255,064 peak charged memory bytes,
6,155,866,048 logical scratch bytes, seven descriptors, and 10,976,692 I/O calls
in 754.155 seconds. The test checks every path, source ID, and parent and samples
provider content once per directory. It creates no payload fixtures and measures
neither image emission nor whole-process/worker RSS.

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

The library's source scanner uses bounded native directory batches (`getdents64`,
`getdirentries`, or `getattrlistbulk`) and descriptor mount queries. Linux
`statx` with `STATX_MNT_ID` distinguishes same-filesystem bind mounts; FreeBSD
and macOS use each live mount's `fstatfs` FSID. Unsupported identity queries are
explicit errors. The ordinary macOS source tests exercise native enumeration and
reopening; they do not create mounts or qualify hostile mount races.

The native macOS compiler reports a 20-byte `attribute_set_t` and an 8-byte
`attrreference_t`. A real `getattrlistbulk` batch decodes its name reference at
offset 24, after the length and returned attribute set. The parser derives
these offsets from the SDK types; see Apple's
[attribute definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/sys/attr.h).
FreeBSD's [nullfs mount implementation](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/fs/nullfs/null_vfsops.c)
allocates a fresh FSID and retains it during `statfs`; the
[VFS allocator](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/kern/vfs_subr.c)
checks live mount IDs for collisions. This source inspection supports the
mechanism choice and does not replace actual-kernel mount qualification.

Confined scanner integration remains open. The Linux prototype denies `statx`;
its source profile needs a tested descriptor-only query without granting
general pathname metadata access. FreeBSD needs the corresponding mount-query
descriptor right. Both profiles need private source-root descriptor adoption
instead of the ordinary library's borrowed-FD duplication. Test these paths
with actual kernels and mount fixtures before claiming a confined source scan.

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
