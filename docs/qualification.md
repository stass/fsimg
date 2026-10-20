# System qualification

These checks exercise the [acceptance contract](../specs/architecture.md#acceptance-contract)
on the hosts identified in [platform evidence](platform-evidence.md). They run
unprivileged with disposable, disk-backed fixtures. They measure named workloads;
they do not establish universal throughput or compatibility with every kernel.
The [build guide](build.md) covers tools, ordinary tests, sanitizers,
interoperability and release/install checks.

## Reproducing the extended checks

From a configured, non-sanitized build on a filesystem with enough scratch space:

```sh
make check-scale
make check-cli-rss
make check-extended
FUZZ_CC=/path/to/llvm/bin/clang make check-fuzz-extended
```

`check-scale` covers metadata sorting/arrays, catalogs, FAT directory reading and
planning, and an actual 128 TiB sparse exFAT file. `check-extended` adds actual
exFAT format planning, extreme fragmentation, fixed-content volume growth and
the CLI memory check. Run its components separately to retain individual logs:

```sh
./test-qualify --catalog 10000000
./test-qualify --wide 100000
./test-fragment --fragments 1048576
./test-exfat-scale --volume 33554432
./test-exfat-scale --volume 33554432000
make check-cli-rss
```

The ten-million-file plan needs about 6.2 GB of scratch. Scratch on a RAM-backed
filesystem consumes system memory outside the library allocation quota. Logical
scratch, sampled allocated blocks, charged library memory and process memory
are separate measurements. Optional process RSS telemetry prints
`rss_available=false` when the host records no sample; zero is not passing RSS
evidence. Output and flush errors fail the qualification.

## Catalog size and directory width

`test-qualify` uses only the public writer API. It reuses one entry, path and
zero-source descriptor, adds files in descending order, and obtains a real
exFAT layout plan. The grouped profile adds 1000 files per legal directory and
sets the entry ceiling to include the directories. It checks accounting,
planned space use and cleanup; it neither emits the image nor retains a
resident oracle. The separate catalog test checks every path, parent and
source reference before this format-planning qualification.

Measured on Intel macOS/Darwin 25.6, Apple Clang 21, APFS:

| Measurement | Grouped format plan | Wide format plan |
| --- | ---: | ---: |
| Files / explicit directories | 10,000,000 / 10,000 | 100,000 / 0 |
| Library budget, bytes | 268,435,456 | 16,777,216 |
| Charged peak, bytes | 5,387,120 | 3,289,968 |
| Logical scratch peak, bytes | 6,155,866,048 | 58,500,368 |
| Sampled physical scratch peak, bytes | 6,155,354,112 | 57,982,976 |
| Owned descriptor peak | 8 | 8 |
| Adapter I/O calls | 12,995,034 | 103,612 |
| Adapter bytes read / written | 20,557,404,999 / 17,284,362,490 | 170,909,822 / 139,002,600 |
| Process RSS peak, bytes | 9,023,488 | 8,400,896 |
| Collection and planning, seconds | 1054.106 | 7.196 |

The grouped plan has 10,010,000 total entries and a 971,301,888-byte volume.
All files are empty: directory allocation and format metadata determine this
volume. This qualifies metadata planning, independently of payload size and
the combined CLI memory gate.

## Fragmented content

`test-fragment` creates an exFAT image with 512-byte clusters, then permutes its
file allocation into even physical clusters followed by odd physical clusters.
Each logical cluster is one extent, with a unique ordinal and known zero bytes.
The fixture mutator uses fixed 64 KiB buffers, independently updates the FAT and
entry checksum, flushes the image, and reopens it read-only before library use.
It retains no FAT, image or fragment list in RAM.

The check reads every byte sequentially, samples random positions, performs
complete structural/payload verification, extracts through a fresh reader, and
compares every extracted byte and file length. It bounds sequential adapter
calls by `16 * fragments + 10000`. This catches chain restarts while allowing
bounded index construction and page-cache traffic.

The APFS run with 1,048,576 fragments transfers a 536,870,912-byte logical file.
At a 16,777,216-byte library budget it reports:

| Measurement | Result |
| --- | ---: |
| Charged peak / logical scratch peak, bytes | 3,651,178 / 75,641,626 |
| Sequential / total adapter I/O calls | 2,117,463 / 6,458,393 |
| Adapter bytes read / written | 1,939,301,444 / 738,260,435 |
| Process RSS peak, bytes | 12,374,016 |
| Read, verify and extraction elapsed, seconds | 46.863 |

Adapter counters and elapsed time exclude raw fixture construction. Reads
include separate content, verification and extraction passes; writes include
scratch and extraction. This is an actual payload test, distinct from sparse
multi-terabyte capacity qualification.

## Volume capacity with fixed live content

The two `--volume` profiles contain the same three files: 4,325,395 logical
bytes and 131,074 initialized bytes. The automatic cluster geometry may change
with capacity. Both use the same 64 MiB allocation limit and reject more than
64 MiB of adapter reads; growing unused space must not cause a payload scan.

| APFS measurement | 33,554,432-byte volume | 33,554,432,000-byte volume |
| --- | ---: | ---: |
| Cluster bytes | 512 | 2048 |
| Charged peak, bytes | 5,539,344 | 5,539,528 |
| Logical scratch peak, bytes | 0 | 2,044,024 |
| Adapter bytes read / written | 317,352 / 148,764 | 2,366,233 / 152,034 |
| Sampled allocated blocks, bytes | 4,612,096 | 6,402,048 |
| Process RSS peak, bytes | 10,805,248 | 10,809,344 |
| Elapsed seconds | 0.015 | 0.313 |

The separate `--huge` profile creates an actual 128 TiB sparse file with an
8 TiB-plus-17-byte zero source and initialized boundary markers at a 64 MiB
budget. APFS and the provided FreeBSD/ZFS host pass. The Linux/ext4 guest
explicitly skips physical creation because its filesystem rejects that size;
logical adapter coverage still runs. Sparse capacity, immediate allocated-block
samples and initialized adapter traffic do not measure settled device writes
or complete multi-terabyte payload transfer. Detailed counts are in
[platform evidence](platform-evidence.md).

## Combined CLI memory

`check-cli-rss` builds a non-installed executable from exactly the production
CLI source list plus a test wrapper. Only `main` and supervisor `waitpid` calls
are redirected. It uses the production library, supervisor, workers, output
relay and confinement. Its Python driver creates 100,000 empty host files in
100 directories plus a 257-byte marker, then runs modes `c`, `i`, recursive
NDJSON `t`, raw `x -O`, payload `W -d` and `x`. The library budget is
16,777,216 bytes; the combined allowance is 83,886,080 bytes. Output goes to
`/dev/null`; bounded measurement lines are streamed from disk logs. Every
extracted file and marker is checked, and scratch must be empty afterward.

On macOS/Linux the wrapper adds the coordinator's self peak to each terminal
child's individual `wait4` peak, including sequential relays. Summing individual
maxima is a conservative upper bound on simultaneous resident memory. It does
not use a child-tree maximum as though it were a sum. Missing/zero peaks,
overflow and failed commands fail this check. Linux reports KiB and macOS
reports bytes; conversion follows the native implementations.
See [Linux getrusage](https://man7.org/linux/man-pages/man2/getrusage.2.html),
[wait4](https://man7.org/linux/man-pages/man2/wait4.2.html), and
[XNU resource accounting](https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/kern/kern_resource.c).

| Host | Largest combined upper bound over all six commands, bytes | Method |
| --- | ---: | --- |
| macOS/APFS | 29,609,984 | Coordinator peak plus sum of child peaks |
| Debian 13 Linux/ext4 | 23,695,360 | Coordinator peak plus sum of child peaks |
| FreeBSD 16/ZFS | 83,886,078 | Enforced sum of three virtual-memory ceilings |

Separate repeated runs can report different peaks. All six operations complete
within the same allowance on each named host. Linux and FreeBSD require the
actual OS sandbox; macOS uses automatic mode with its explicit unavailable
warning. Sanitizer RSS is outside this non-sanitized acceptance measurement.

FreeBSD's [statistics clock](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/kern/kern_clock.c)
can miss short relays, so zero sampled peaks cannot establish a resident-memory
bound. Its qualification bootstrap re-execs the same program under a uniform
27,962,026-byte hard/soft `RLIMIT_AS` and an 8 MiB stack-growth ceiling. The
bootstrap is an observer outside the measured execution. Every coordinator and
internal-role entry asserts that its initial `kinfo_proc.ki_size` is within the
finite inherited cap before production code runs. Ordinary later mappings,
heap and stack growth obey the cap. The supervisor's closed role enum has a
compile-time assertion tying it to at most three simultaneous processes.
Resident memory cannot exceed their combined virtual-memory ceiling.

The initial-map assertion matters: exec's initial mappings are not all subject
to the ordinary mapping checks. A negative fixture maps and touches 128 MiB
before lowering limits. It demonstrates that checking limit values alone
falsely succeeds, and requires rejection specifically at the initial-map
assertion. The guard runs on FreeBSD and explicitly skips elsewhere. This is
an enforced VM upper bound for the stated qualification profile, not a measured
RSS peak or a production memory-limit policy. The relevant native contracts
are [getrlimit](https://raw.githubusercontent.com/freebsd/freebsd-src/main/lib/libsys/getrlimit.2),
[process accounting](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/kern/kern_proc.c),
[ELF loading](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/kern/imgact_elf.c),
[exec](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/kern/kern_exec.c),
and [VM mapping](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/vm/vm_map.c).
On the tested kernel revision, [data-segment growth](https://raw.githubusercontent.com/freebsd/freebsd-src/975e3605ebb15cbaf5a25c1c9d1f51aed41291d0/sys/vm/vm_unix.c)
also rejects aggregate map growth above `RLIMIT_VMEM`, and the
[resource definitions](https://raw.githubusercontent.com/freebsd/freebsd-src/975e3605ebb15cbaf5a25c1c9d1f51aed41291d0/sys/sys/resource.h)
make `RLIMIT_AS` its alias. This bound does not depend on allocator choice.

## Sustained fuzzing and release checks

Homebrew LLVM 23.1.2 instruments every library object and harness with coverage,
ASan and UBSan. Three independent 600-second campaigns complete without an
unintended sanitizer or accounting report:

| Corpus | Seed | Inputs executed | Actual seconds |
| --- | ---: | ---: | ---: |
| FAT12/16/32 | 271828 | 281,960 | 601 |
| exFAT raw/compressed upcase | 314159 | 302,479 | 601 |
| Worker protocol | 161803 | 22,292,756 | 601 |

The format input ceiling is 3 MiB with a ten-second per-input timeout; the
protocol ceiling is 64 KiB with a five-second timeout. Reader budgets, entry,
depth and work limits remain finite. Parser/protocol fuzzing complements the
actual worker lifecycle tests; it does not establish complete path coverage.
`FUZZ_SECONDS=1 make check-fuzz` separately exercises the script's finite-time
dispatch and succeeds on repeated invocation with existing seeds in all three
corpora; invalid or oversized values fail before building.

The ordinary 26-test suite, supported sanitizer matrix, private-prefix
installation, C/C++ static/shared consumers, native runtime-dependency audit,
`make distcheck`, and release-archive build with Autotools generators denied
are described in [build.md](build.md). Third-party development oracles remain
outside the core and default CLI. The [reference manifest](references.md)
records the licenses and attribution for adapted code and generated tables.
The required local architecture/security/readability review and read-only
OpenCode review use the criteria and exact model in the
[implementation plan](implementation-plan.md). Actionable residual work is
kept in [TODO.md](../TODO.md).

The final system review and substantive follow-ups leave no Critical, High,
Medium or actionable readability finding in this qualification work. The
FreeBSD limit proof is checked against the pinned kernel sources above;
reproducible fixture failures receive failing-then-passing checks. The UFS
publication handoff investigation and Low residuals remain explicit open work.
