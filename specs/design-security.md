# Security, extraction, and confinement

## Threat model and guarantees

An attacker can control every byte of an image, its names and metadata, its
fragmentation and directory graph, and the behavior of a supplied input tree.
Images may be truncated or modified concurrently. A parser defect may give the
attacker arbitrary code execution inside the CLI worker. The design therefore
requires both correct validation and limited worker authority.

Trusted components are the host kernel, fsimg's installed executable and
invocation environment, the resource/path/publication layers, and descriptors
explicitly supplied by the caller. Application callbacks are trusted code,
not bytes validated by the library. A malicious process already running as
the same user, an administrator changing mounts, or a compromised kernel is
outside the confinement guarantee. Private output directories do not isolate
against another process with the same credentials.

With the image-reading/extraction sandbox profile active, a worker cannot
access unrelated host contents, write outside its output/scratch capabilities,
use the network, execute another program, or acquire new ambient authority.
Creation additionally has source-root authority with the limits described below.
A worker can corrupt its
authorized output after compromise; a sandbox does not authenticate the
extracted contents or prove that the parser is correct. Applications must
treat extracted files as untrusted data and not automatically execute them.

Without OS confinement, path validation and resource checks still apply to
correctly executing code, but process separation alone does not contain a
memory-corruption exploit. In-process library calls have the same limitation.
The CLI explicitly reports which case applies.

## Sandbox policy

The CLI accepts `-S auto`, `-S required`, and `-S off`:

| Policy | Available supported profile | Known unavailable mechanism | Unexpected or partial setup failure |
| --- | --- | --- | --- |
| `auto` (default) | Install and verify it | Warn on stderr before parsing; continue unconfined | Fail the operation |
| `required` | Install and verify it | Fail before parsing | Fail the operation |
| `off` | Explicitly run without OS confinement and report that choice | Same | Not applicable |

Automatic mode never silently retries an operation without confinement after
an enforcement error. Unsupported kernel facilities, disabled facilities, or
an unsupported platform/profile are classified explicitly. Ambiguous errors
are failures. A policy is reported as active only after every required layer
is installed, not because one syscall succeeded. Machine-readable results
include policy, backend, and active/unavailable/disabled status.

The policy applies to all image-consuming commands, including `info`, `ls`,
`cat`, and `verify`; probes are parsers too. Creation uses a separate profile
with read access to the selected source roots. `formats` and usage output do
not require an image-processing worker.

## Coordinator and worker

The coordinator parses trusted command syntax, opens input/source capabilities,
prepares private scratch/output roots, and starts a worker from the same
installed executable. This requires no separately installed helper tool. A
fresh execution with a minimal environment removes inherited application
memory and unneeded environment variables from the worker. No shell or PATH
lookup selects an executable.

Only designated descriptors survive worker startup: read-only input or source
roots, scratch authority, fresh output authority where needed, and bounded
control/data channels. Other descriptors, sockets, terminal handles, and
executable handles are closed. Core dumps are disabled. Runtime initialization
needed by libc occurs before confinement; no locale, timezone, or plugin
loading may cause ambient file opens while processing an image.

The worker establishes its profile and reports readiness and confinement status.
It waits for the coordinator's acknowledgement before reading image data or
scanning source names. The coordinator emits any automatic-mode warning before
acknowledging, so the warning precedes parsing. The coordinator never
parses image headers and never opens a host path requested by the worker.
Image output staging directories can be prepared before this point; the image
file itself is created only after successful planning. Source scanning rejects
encounters with scratch/output objects rather than importing them.

The worker writes payload bytes directly through authorized descriptors.
Gigantic image payloads do not pass through a per-file request broker. `cat`
uses a separate bounded data pipe for explicitly requested raw bytes. Control
messages use a versioned framing protocol with a fixed maximum length,
known message types, bounded strings, checked counters, and a closed state
machine. No worker-supplied descriptor or pathname is accepted as new authority.

The coordinator validates the selected entry before forwarding raw cat bytes to
a same-executable output relay. That relay receives only its bounded input and
status channels and the caller's stdout/stderr sinks. It owns potentially blocking sink
writes; the coordinator supervises and terminates both children on cancellation.
Neither child changes borrowed sink flags. Formatted command output and warnings
use the same relay; warnings are confirmed before parsing is acknowledged.
Success requires exact confirmed transfer and successful exit of both children.

The coordinator validates protocol lengths before allocation, escapes names
and control characters before terminal output, and never treats worker text
as a format string. A worker cannot forge a successful publication by sending
an early success frame: the coordinator also requires the correct state,
successful worker exit, and successful final output checks. Truncated messages,
unknown types, duplicate completion, child signals, and EOF before completion
are errors. Cancellation and parent death stop new work when observed at host
I/O boundaries. An in-flight kernel operation can complete or remain blocked
according to host behavior. The output relay holds only one bounded record;
after coordinator death its next status write fails, preventing another record.

Large listing names use bounded entry-start/name-chunk/entry-end records.
The coordinator enforces the total path limit and legal message order while
escaping text incrementally. A 64 KiB path need not fit, with framing or JSON
escaping, in one 64 KiB message. Diagnostic paths may be explicitly truncated;
listing names may not be silently truncated. This protocol adds no request for
host path access.

## Source-root profile

The creation worker can open objects beneath explicitly authorized source roots.
This is more authority than a pre-opened regular image descriptor. Normal
scanning uses nonblocking, no-controlling-terminal, no-follow opens and checks
the opened object's type before reading; FIFOs cannot make an ordinary open
wait for a writer, and special files are rejected. A prior directory type check
alone cannot prevent replacement before open.

Opening a device can have effects before `fstat()` rejects it. Neither a
read-only directory capability nor a filesystem path allowlist universally
prevents a compromised worker from opening a preexisting device beneath that
root. Do not advertise device exclusion for this profile without a tested
type-restricted opening mechanism. The profile still denies networking,
execution, ioctl access, metadata mutation, and writes outside output/scratch.
It is named and reported separately from the image-reading profile.

Scanning rejects descendant mounts by comparing each opened object's device and
native mount identity with its authorized root. An unavailable mount-identity
query returns unsupported; matching device numbers alone are insufficient for
same-filesystem mount aliases. Post-open checks do not prevent adversarial mount
changes during resolution, and the security claim excludes those changes. For
adversarial source trees requiring device-open exclusion, use an isolated
source filesystem mounted with device
nodes disabled, or a tested stronger host mechanism. A supported sandbox in
`required` mode guarantees the advertised profile, not stronger authority
restriction than its source roots allow. Capability output and user docs state
this limitation explicitly.

## Platform profiles

### FreeBSD

Use Capsicum capability mode with explicit descriptor rights, ioctl limits,
and fcntl limits. Entering capability mode blocks ambient namespace access;
descriptor rights determine what operations remain possible. Input capabilities
exclude mutation, output capabilities exclude execution and unrelated lookup,
and no network descriptor is inherited. Do not rely on `cap_enter()` alone.
The design follows the contracts in the FreeBSD
[capability-mode manual](https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD+14.3-RELEASE+and+Ports&query=cap_enter&sektion=2)
and [Capsicum overview](https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD+13.3-RELEASE&query=capsicum&sektion=4).

### Linux

Combine Landlock filesystem restrictions, `no_new_privs`, and an architecture-
checked seccomp allowlist. Landlock constrains path access; seccomp removes
network, execution, process-control, mount, and unnecessary syscall authority.
Require a tested Landlock ABI with truncation protection (at least ABI 3) and
every facility required by the selected profile. Pre-opened descriptors must
be limited separately: Landlock does not revoke their existing authority.
These constraints follow the kernel's
[Landlock documentation](https://docs.kernel.org/userspace-api/landlock.html).

Seccomp alone is not filesystem confinement and cannot inspect path strings.
Use an allowlist of operations the worker actually needs, with architecture
and alternate-ABI checks, rather than a denylist of dangerous syscall names.
The kernel documents both limitations in
[Seccomp BPF](https://docs.kernel.org/userspace-api/seccomp_filter.html).

Landlock does not mediate every inode-metadata operation. The portable CLI
profile therefore denies ownership, permission, xattr, and timestamp mutation
syscalls, and path-based metadata queries that could bypass confinement. It
uses safe creation modes, opened-object metadata queries, and no arbitrary
`O_PATH` access after setup. This includes checking libc's actual syscall forms
on each supported architecture. `io_uring`, descriptor acquisition through
other processes, hardlink creation, and unnecessary fd duplication paths are
not part of the worker profile. New syscall needs require a security review.

The source profile requires the descriptor-only Linux statx form: a NULL path,
AT_EMPTY_PATH, and an explicit descriptor other than AT_FDCWD. Setup proves
that form is supported before restricting the process; an older kernel reports
the source profile unavailable. Seccomp checks both halves of the path pointer
and flags and denies every pathname form. Unconfined library callers retain an
empty-string fallback for older kernels. FreeBSD source capabilities include
descriptor mount queries, so mount identity checks require no ambient lookup.

### macOS

The standalone CLI has no claimed equivalent built-in backend. Apple's
supported [App Sandbox model](https://developer.apple.com/documentation/security/app-sandbox)
uses entitlements; its
[helper-tool guidance](https://developer.apple.com/documentation/xcode/embedding-a-helper-tool-in-a-sandboxed-app)
concerns tools embedded in a sandboxed application. The SDK's `sandbox.h` marks
`sandbox_init()` unsupported/deprecated. Private profile syntax and
`sandbox-exec` are not an installation or security dependency.

Consequently, `auto` warns and operates with validation/path confinement on
standalone macOS, while `required` refuses. A signed application distribution
or another supported mechanism can supply a separately reviewed backend; it
is not inferred from the mere existence of a sandbox-related API. A host
application's sandbox is not automatically advertised as the fsimg profile.

## Safe extraction

The destination operand names a new directory, not a tree to merge into.
The coordinator creates it exclusively with mode 0700 and passes only its
opened directory capability to the extractor. Any existing object, including
an empty directory or symlink, fails. The selected parent comes from the user;
image names never influence how that initial authority is obtained.

Image paths are split into validated components. Reject embedded NULs, `.` and
`..`, empty internal components, embedded separators in a component, invalid
encodings, excess length/depth, and names that the host cannot represent
without alteration. The single leading `/` denotes the image root and is
never passed to host path resolution. A selected subtree is matched on
component boundaries, not by a string-prefix test.

All descendant creation uses directory-relative operations. Open each directory
with no-follow protection and verify its type before using it as authority.
Regular files use exclusive creation and no-follow flags with mode 0600.
On Linux, `openat2()` beneath/no-symlink/no-magic-link resolution is additional
protection where available; the
[manual](https://man7.org/linux/man-pages/man2/openat2.2.html) distinguishes
these guarantees from final-component-only `O_NOFOLLOW`. The portable fallback
walks one component at a time beneath the fresh private root.

The safe profile creates only regular files and directories. It rejects
symlinks, hardlinks, devices, sockets, FIFOs, reparse-like objects, and unknown
critical entry kinds. It does not restore owners, setuid/setgid bits, executable
permissions, ACLs, xattrs, or host-specific privileged flags. Host case-folding
or normalization collisions fail through exclusive creation; they are never
resolved by overwrite or automatic renaming.

The CLI's content profile extracts bytes and hierarchy with private modes.
Image timestamps and attributes remain available through inspection and the
library metadata API, but this profile does not mutate host timestamps. The
in-process library can apply caller-selected safe timestamp preservation using
opened file descriptors; that is not a promise of a portable CLI sandbox
capability. Adding such a CLI capability requires proving that metadata calls
cannot mutate read-only input or escape through a descriptor alias.

Count logical extracted bytes, including zero ranges, before creating or
extending files. Default total output is at most the selected volume length
for the supported uncompressed formats. A caller can select another finite
ceiling. Sparsity, cross-links, and repeated entry references do not evade it.
Disk-full and short-write errors fail the operation. Completion requires
successful file finalization; sparse logical length alone does not prove that
initialized content was written.

On failure, the new destination may contain partial files. Leave it private,
report its location and incomplete state, and never announce success or merge
it with a preexisting tree. Automatic recursive deletion is not required: an
unrestricted cleanup walk over worker-controlled names would enlarge the
trusted code. An empty directory created before a startup failure may be removed
after verifying that it is still the created object. Directory-wide atomic
publication and resumable extraction are outside this contract.

## Image publication

Image creation uses a private staging directory on the destination filesystem.
The worker can create the staged image there but cannot access the final
parent directory. After the worker exits successfully, the coordinator verifies
that the staged result is the expected regular file with the expected size,
using no-follow operations. It finalizes file data before publishing.

Publication never replaces an existing name. Use a supported atomic no-replace
rename, or regular-file `linkat()` publication followed by removal of the
private name. Both names refer to the same inode; no second image copy is
made. A check-then-ordinary-rename sequence is forbidden. Where neither safe
primitive is supported, return unsupported and retain/report the staged result;
do not fall back to overwriting. Platform/filesystem support is tested, not
assumed from headers. Linux documents filesystem-dependent no-replace support
in its [rename manual](https://man7.org/linux/man-pages/man2/rename.2.html).

Then sync the destination directory where supported to complete the durability
contract. Outcomes are distinguished:

| Outcome | Report |
| --- | --- |
| Failure before publication | Final name absent or unchanged; staged partial/result may remain private |
| Final name already exists | `FSIMG_EEXISTS`; existing object untouched |
| Publication succeeds, durability completes | Success |
| Publication succeeds, directory sync/final cleanup fails | Output exists; error identifies durability or cleanup uncertainty; no blind retry |
| Filesystem reports an ambiguous publication error | Explicit uncertain-publication result; inspect identity, do not overwrite or retry blindly |

A remote filesystem can have weaker or ambiguous failure semantics. fsimg does
not claim stronger guarantees than its adapter can establish. Publication
status is independent of worker success. Power loss may leave private staging;
stale staging cleanup must never follow symlinks or delete files by a broad
filename pattern.

## Other boundaries

Reopen host sources relative to authorized roots, without following symlinks,
and validate identities and lengths around copying. Output and scratch must
not be imported as sources. A source changed in place can defeat ordinary
timestamp-based detection; the source stability contract requires a snapshot
when an adversarial concurrent writer is relevant.

Decoders validate lengths, offsets, integer products, chain termination,
directory references, encoding, and required checksums before use. In exFAT,
uninitialized logical file tails are returned as zero; they are not read from
old cluster contents. Emitters initialize all reserved bytes and padding.
Diagnostics never dump uninitialized buffers, file payloads, pointers, or
unbounded attacker strings.

Normal input-induced resource exhaustion is controlled by allocation, scratch,
descriptor, output, and traversal budgets. These are correctness checks, not
an enforcement boundary after arbitrary code execution. OS limits restrict
descriptors, individual file sizes, and configured CPU/time use where available.
A deployment requiring hard aggregate disk/memory ceilings against a compromised
worker also uses host quotas or equivalent containment. The sandbox's filesystem
allowlist alone does not guarantee a disk-space quota.

## Required security evidence

* Boundary fixtures cover arithmetic wrap, slice escape, unaligned fields,
  early EOF, cyclic chains/directories, overlapping extents, malformed encodings,
  stale-data tails, and hostile entry counts.
* Extraction fixtures attempt `..`, absolute host paths, embedded separators,
  host normalization collisions, source/output aliasing, symlink pivots, hardlink
  attacks, special files, and privileged metadata. Outside sentinels must retain
  both contents and metadata.
* A dedicated adversarial worker harness attempts forbidden syscalls after
  confinement, including metadata mutation through input descriptors, alternate
  syscall ABIs, network/exec, `/proc` descriptor reopening, and inherited-fd leaks.
  These tests do not depend on finding a parser exploit.
* Protocol fuzzing covers lengths, truncation, reordered/duplicate messages,
  invalid counters, terminal escapes, forged completion, and cancellation.
* Tests simulate absent sandbox support separately from installation failure,
  and verify all three policies, warning visibility, and machine-readable status.
* Fault injection covers every stage of allocation, scratch spill, file creation,
  data writes, flush, publication, directory sync, worker death, and signal
  handling. Assertions include final-name state, output privacy, and leaked
  descriptors/temporary files, not only return codes.

The implementation plan makes these checks release gates. Platform sandbox
tests are required on their actual kernels; a mocked syscall return cannot
establish confinement.
