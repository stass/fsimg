# Platform mechanism evidence

The mechanism harnesses are `test-sandbox`, `test-sandbox-setup`,
`test-publish`, and `test-worker`. Kernel, compiler, and filesystem identify
execution evidence; compiled platform branches alone do not establish
enforcement. CI requires confinement on Linux and FreeBSD rather than accepting
a sandbox SKIP.

| Mechanism | Scope exercised | Host evidence |
| --- | --- | --- |
| macOS confinement | Explicit unavailable result, automatic warning before ACK, required refusal, explicit off | Darwin 25.6.0 x86_64, Apple Clang 21.0.0, APFS |
| Linux confinement | Landlock ABI >= 3, seccomp, image and source workers, forbidden operations | Debian 13, Linux 6.12.113+deb13-amd64 x86_64, Clang 19.1.7, ext4 |
| FreeBSD confinement | Capsicum descriptor/ioctl/fcntl rights, image and source workers, forbidden operations | FreeBSD 15.1-RELEASE-p5/UFS and user-provided FreeBSD 16.0-CURRENT/ZFS, amd64, Clang 19.1.7 |
| Source mount boundaries | Required-confined scans reject live descendant mount aliases before PLAN | Linux bind mount with equal device IDs; FreeBSD nullfs mount with distinct FSIDs |
| Regular-file publication | Exclusive hardlink, staged unlink, directory syncs, failure/race reporting | APFS, ext4, UFS, ZFS; UFS immediate-reader identity qualification remains open |

Linux and FreeBSD 15.1 run in local QEMU guests booted from checksum-verified
upstream images. The user-provided FreeBSD host is 16.0-CURRENT, kernel
`main-n285245-975e3605ebb1`, on ZFS; it runs the primary native FreeBSD checks.
The library and ordinary suites run unprivileged. Only the
separate mount qualification prepares bind/nullfs fixtures with guest root;
its scanner remains unprivileged. Both fixtures preserve outside sentinel
bytes. Linux reports devices 65025/65025 and mount IDs 29/68; FreeBSD reports
devices 61/687931137 and FSID-derived mount IDs
7650454420974902357/2954641735315095593. These IDs identify those live fixtures,
not stable identifiers across boots.

The ordinary suite contains eighteen tests. macOS reports sixteen passes and
two explicit skips (unavailable backend and Linux-only setup-fault harness).
Linux reports seventeen passes and one integration skip because fatcat is
unavailable there. FreeBSD 16/ZFS reports
seventeen passes and one skip (Linux-only setup-fault harness), including
native FAT interoperability. Native
ASan/UBSan and additional platform sanitizer outcomes are recorded with their
commands in [build.md](build.md). Standalone LSan/MSan are unsupported on macOS
and FreeBSD; a skip is not a pass. Fresh confined children disable tracer-based
leak scanning, while parent/library leak checks remain enabled where supported.

Publication tests cover the named filesystems, not remote filesystems or crash
recovery. FreeBSD/UFS can change ctime after publication without an observed
payload change. Readers conservatively reject that identity change. Inode
fsync and a no-replace rename experiment do not establish a stable handoff;
`test-publish --identity` is a separate qualification probe and passes on the
provided ZFS host. The unresolved UFS investigation is tracked in `TODO.md`.

FAT qualification uses mtools 4.0.49, dosfstools 4.2 on macOS/Linux, native
FreeBSD `newfs_msdos`/`fsck_msdosfs`, and the pinned fatcat oracle to create,
populate, and independently check FAT12, FAT16, FAT32, and a FAT16 image with
4096-byte sectors
and 8192-byte clusters. fsimg enumerates their Unicode names, compares file
bytes, and completes structural plus payload verification. The FAT12 image
contains forced fragmentation; the FAT32 image confirms cluster zero in a
subdirectory's root-parent `..` record. Synthetic fixtures cover 96 legal
geometry combinations, boundary cluster counts, corruption and failure paths,
and a sparse 4 GiB-minus-one-byte file whose boundary bytes and allocation are
checked without reading the complete payload.

Fatcat comparisons cover ordinary ASCII names, ASCII long names, nested paths
and the forced fragmented file. Its pinned reader rejects non-512-byte sectors;
that subcheck explicitly skips while the formatter/checker, mtools and fsimg
still validate the wide-sector fixture. Unicode content is checked through
mtools. Fatcat information output is logged but does not determine geometry or
free-space correctness. The FreeBSD build uses only the minimal native-interface
patch described in [references](references.md).

Native formatter fixtures specify sector count and conservative FAT capacity
explicitly. Automatic fsimg geometry follows Microsoft's 16-cluster cutover
margin; exact boundary counts remain legal and tested. This avoids FreeBSD's
4086/65526 checker classification at the Microsoft 4085/65525 boundaries.
The native checker can warn that an FSInfo next-free hint points at an allocated
cluster after mtools population; the hint is a search starting point, and images
are not repaired or rewritten to silence that advisory warning.

The fully instrumented libFuzzer smoke run uses Homebrew LLVM 23.1.2 with
ASan/UBSan and library coverage. It completes 1000 mutations with all three FAT
seed variants and finite reader budgets (`cov: 2076`, `ft: 5758`) and 1000
protocol mutations (`cov: 345`, `ft: 654`), with no unintended sanitizer or
accounting findings. This is finite parser
exercise, not sustained fuzzing or an exhaustive compatibility claim. See
[build commands](build.md) for instrumentation and logical zero-tail details.

The FAT directory scale fixture enumerates and structurally verifies 65,520
empty files with a 16 MiB context budget and one shared cache page. It reports
2,190,928 peak charged memory bytes, 28,632,432 logical scratch bytes,
28,639,232 sampled physical scratch bytes, five descriptors, and 2,134,938 I/O
calls in 104.821 seconds. Process `ru_maxrss` is 5,382,144 bytes. This exercises
metadata spilling above the memory budget; it does not measure payload
throughput or qualify CLI/worker RSS.

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
explicit errors. Actual source tests on all three hosts exercise native enumeration and
reopening. Linux and FreeBSD also execute the source scan inside required
confinement, including the descendant-mount rejection fixtures above. This
does not qualify hostile concurrent mount races.

The native macOS compiler reports a 20-byte `attribute_set_t` and an 8-byte
`attrreference_t`. A real `getattrlistbulk` batch decodes its name reference at
offset 24, after the length and returned attribute set. The parser derives
these offsets from the SDK types; see Apple's
[attribute definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/sys/attr.h).
FreeBSD's [nullfs mount implementation](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/fs/nullfs/null_vfsops.c)
allocates a fresh FSID and retains it during `statfs`; the
[VFS allocator](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/kern/vfs_subr.c)
checks live mount IDs for collisions. Source inspection supports the mechanism
choice; the live nullfs fixture provides separate kernel execution evidence.

Confined source scans adopt prepared source and scratch descriptors without
requiring duplication after restriction. Linux preflights the descriptor-only
`statx(fd, NULL, AT_EMPTY_PATH, STATX_MNT_ID, ...)` form before installing the
profile. The filter accepts only a null pointer (both halves), exact
`AT_EMPTY_PATH`, and a descriptor other than `AT_FDCWD`. Older kernels lacking
that form report the source profile unavailable before restriction. Pathname,
empty-string, high-pointer-half, and high-flags-half bypass attempts are denied
in the kernel tests. FreeBSD grants `CAP_FSTATFS` only for the source role.

Worker descriptor cleanup uses macOS close-on-exec-default spawn policy or
`posix_spawn_file_actions_addclosefrom_np` on Linux and FreeBSD. Failure to
establish complete closure prevents worker startup; resource limits
cannot bound a fallback loop because descriptors survive a lowered limit. A
regression retains a high descriptor, lowers the limit, and checks that cleanup
either closes that descriptor or refuses to claim success.

FreeBSD limits input to reading/seek/stat and flag inspection, output roots to
creation/read/write/seek/stat/sync/truncation, scratch roots additionally to
removal, and optional source roots
to lookup/read/seek/stat/mount identity. No ioctl or metadata-mutation right is
granted; input timestamp mutation is explicitly tested and denied.
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
