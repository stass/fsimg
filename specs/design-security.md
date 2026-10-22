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

The policy applies to all image-consuming modes, including `i`, `t`, `x -O`
and `W`; probes are parsers too. Creation uses a separate profile with read
access to its source roots, one per create operand and, for a manifest
operand, the manifest and the content directory `-C` gives it, if any.
`--formats` and usage
output do not require an image-processing worker.

## Coordinator and worker

The coordinator parses trusted command syntax, opens input/source capabilities,
prepares private scratch/output roots, and starts a worker from the same
installed executable. This requires no separately installed helper tool. A
fresh execution with a minimal environment removes inherited application
memory and unneeded environment variables from the worker. No shell or PATH
lookup selects an executable.

Only designated descriptors survive worker startup: the image input, which
is a read-only regular file or the pipe through which the coordinator pumps
standard input unread (the [CLI design](design-cli.md#coordinator-and-streaming-representation)
states that rule and refuses a terminal), source roots, scratch authority,
fresh output authority where needed, the unlinked
[handed-over files](design-layers.md#handed-over-files), the staged
[`x --mtree` manifest](design-mtree.md#extraction-writing-a-manifest), and
bounded control/data channels. The sandbox's input check admits a regular file or a
pipe and nothing else. Other descriptors, sockets, terminal handles, and
executable handles are closed. Core dumps are disabled. Runtime initialization
needed by libc occurs before confinement; no locale, timezone, or plugin
loading may cause ambient file opens while processing an image.

The worker's authority over names is this mapping and nothing more. In the
staging directory it may create the staged output and remove nothing. In
the scratch tree it may create and unlink its own scratch files. A
handed-over file (spool, raw intermediate) is a descriptor only, with no
name to create or remove. The staged manifest of `x --mtree` is a
write-only descriptor of one file the coordinator created in its own
private staging directory: the worker may write, resize and flush it, and
holds no name or directory through which to rename, replace or read it.
Source roots and the image input are read-only.
The platform profiles below install exactly these rights.

The profiles bound each grant to its directory, not to the names within
it: the worker may create any regular file in staging, create and unlink
any file in the scratch tree, and create any file or directory beneath an
extraction destination. Three naming constraints therefore rest on the
worker's own behaviour, with the coordinator's checks behind them. The
staged output is the single file `image`, the only name the coordinator
publishes or opens for verification; another entry in staging is never
published and only keeps the private staging directory from being removed.
Scratch holds only the worker's own files, which the coordinator removes
whatever their names. The manifest's content rests on the worker too: a
compromised worker can write any bytes and any length into it, which the
coordinator publishes under the user's `--mtree` name with mode 0600, just
as it publishes any staged image's bytes; it cannot choose the name, the
directory or the mode. Extracted names are the validated, host-representable
names the extractor derives from the image. A compromised worker can misuse
names within its grants but cannot reach beyond them.

The worker establishes its profile and reports readiness and confinement status.
It waits for the coordinator's acknowledgement before reading image data or
scanning source names. The coordinator emits any automatic-mode warning before
acknowledging, so the warning precedes parsing. The coordinator never
parses image headers and never opens a host path requested by the worker.
Image output staging directories can be prepared before this point; the image
file itself is created only after successful planning. Source scanning rejects
encounters with scratch/output objects rather than importing them.

The worker writes payload bytes directly through authorized descriptors.
Gigantic image payloads do not pass through a per-file request broker. `x -O`
uses a separate bounded data pipe for explicitly requested raw bytes. Control
messages use a versioned framing protocol with a fixed maximum length,
known message types, bounded strings, checked counters, and a closed state
machine. No worker-supplied descriptor or pathname is accepted as new authority.

The coordinator validates the selected entry before forwarding raw `x -O` bytes to
a same-executable output relay. That relay receives only its bounded input and
status channels and the caller's stdout/stderr sinks. It owns potentially blocking sink
writes; the coordinator supervises and terminates both children on cancellation.
Neither child changes borrowed sink flags. Formatted command output and warnings
use the same relay; warnings are confirmed before parsing is acknowledged.
Success requires exact confirmed transfer and successful exit of both children.

Standard input reaches the worker through the mirror-image input relay, a
same-executable child holding only the caller's standard input, the write
end of the worker's input pipe, and the null device for its own output.
Like the output relay it runs unconfined: it interprets nothing, its two
descriptors are all the authority it has, and a sandbox would remove
nothing it holds. It disables core dumps, ignores SIGPIPE, waits on a
non-blocking standard input rather than failing, changes no flags on the
borrowed descriptor and exits non-zero on any failed transfer, with a status
of its own when standard input itself failed a read. The coordinator
supervises it with the worker; a stream read succeeds only when the relay
exited successfully after delivering end of file, and a failed read of
standard input is reported as the cause, whatever the worker concluded from
the bytes before it. The relay closes the worker's pipe only by exiting, so
its status is final by the time the worker can see the early end. A closed
standard descriptor is held from startup with the null device opened the
opposite way, so it fails every use as a closed one does while no
descriptor the coordinator opens later can take its number and be handed
to a relay as the caller's stream.

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
The coordinator opens one root per create operand; for a
[manifest](design-mtree.md#security-and-bounds), the manifest file and,
when a `-C` gives it one, its content directory, with the same no-follow,
nonblocking, no-controlling-terminal open that scanning uses, and passes the
descriptors as the worker's complete source authority: the worker never
receives the `-C` directory, an operand's parent, or a pathname to resolve,
and every check below applies to each root on its own. Roots may nest or
overlap on the host; what that does to the image is the
[merging rule's](design-sources.md#merging-roots) business, which fails two
roots that supply the same file, and it grants nothing beyond the union of
the roots. This is more authority than a pre-opened regular image
descriptor. Normal
scanning uses nonblocking, no-controlling-terminal, no-follow opens and checks
the opened object's type before reading; FIFOs cannot make an ordinary open
wait for a writer, and special files are rejected. A prior directory type check
alone cannot prevent replacement before open, so an object is opened before
its kind is checked, on every host and including the file a manifest names,
which must then be a regular file; a name is inspected only to classify a
failed open or to read a link.

Opening a device can have effects before `fstat()` rejects it. Neither a
read-only directory capability nor a filesystem path allowlist universally
prevents a compromised worker from opening a preexisting device beneath that
root. Do not advertise device exclusion for this profile without a tested
type-restricted opening mechanism. The profile still denies networking,
execution, ioctl access, metadata mutation, and writes outside output/scratch.
It is named and reported separately from the image-reading profile.

Scanning crosses mount points as tar does: a filesystem mounted beneath a
source root, a bind or nullfs alias included, is authorized content of that
root, and the source grants already let the worker read it. The mount table
beneath a root is trusted as the root's ownership is: on a host where an
unprivileged user may mount, such as FreeBSD with `vfs.usermount` or a FUSE
filesystem mounted with `allow_other`, that user chooses content the scan
reads with the invoking user's credentials, or can block it; image a
snapshot instead. Crossing a network or automounted filesystem performs its
I/O on the worker's behalf. The opened-object checks still apply on every
filesystem: only regular files, directories and symbolic links are
imported, and nothing that is the output or scratch directory by device and
inode.
Depth and entry limits bound the traversal; they do not bound a filesystem
operation that blocks. For adversarial source trees requiring device-open
exclusion, use an isolated source filesystem mounted with device nodes
disabled (`nodev`), and no descendant mount that allows them, or a tested
stronger host mechanism. A supported sandbox in
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
Importing a manifest needs no path-based query: the file it names is opened
and then checked, as the source-root profile states.

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

The destination is one directory that the coordinator has proven safe from
other writers before the worker starts, and the worker receives only its
opened descriptor, never the parent. There are two ways to obtain it, and the
[CLI contract](design-cli.md#extraction) defines their syntax:

* By default the coordinator creates a fresh directory, named after the
  image stem, inside the existing `-C` directory: one `mkdirat` with mode
  0700 beneath the opened parent, then a no-follow reopen of the result. An
  existing object of that name, including an empty directory or a symbolic
  link, fails. A name fsimg cannot derive is not invented; the user then
  chooses the second way.
* With `-E` the user designates the destination itself. The coordinator opens
  it with `O_DIRECTORY | O_NOFOLLOW`, so a symbolic link fails, and with it
  any other existing object that is not a directory, both as
  `FSIMG_EUNSUPPORTED`; it uses only that descriptor afterwards: `fstat`
  must show a directory owned by the effective user without group or
  other write permission; a read of the directory through the descriptor
  must find it empty; and the access
  control list must be trivial, as defined next. This accepts exactly the
  state a fresh directory is in, so the worker's behaviour is the same for
  both: every entry is created exclusively and no-follow beneath a verified
  parent, and an object another process adds concurrently is a collision
  failure, never an escape.

Both destinations satisfy one rule against other writers, checked on the
opened descriptor by a compat-layer function. The rule protects integrity,
not confidentiality: another principal who can write the directory can
swap it for another directory between its creation and its opening, plant
objects the extraction collides with or races, or replace a staged name
between its validation and its publication. The mode bits are fully
inspectable, so they are judged: the owner is the effective user, and group
and others have no write bit. Read and search bits for others are allowed:
they let others list the top-level names of an `-E` destination, and who
can read the contents is decided by the filesystem's permissions, not by
this check. fsimg creates every extracted directory 0700 and every file
0600, but a filesystem that ignores the creation mode, such as Linux VFAT
mounted with `umask=022`, shows its mount's permissions instead. An access
control list is not interpreted, so any non-trivial ACL of any type the
platform can report on the descriptor is refused, one that grants only
reading included: on FreeBSD
`acl_is_trivial_np` for the access and NFSv4 types the filesystem supports,
and any entry at all in a POSIX.1e default ACL, which has no trivial form; on Linux the presence of a
`system.posix_acl_access` or `system.posix_acl_default` attribute in
`flistxattr`, which the kernel keeps only for a non-trivial ACL; on macOS
any extended ACL entry. On Linux a listed `system.nfs4_acl` attribute is
refused as well, trivial or not: the NFS client lists it for every inode of
a server with ACL support, and fsimg cannot prove it trivial without
decoding NFSv4 ACLs, which is out of scope. A Linux NFSv4 destination on
such a server therefore cannot be used, a limitation the user-facing
documentation states; FreeBSD's NFSv4 case is unaffected, since
`acl_is_trivial_np` answers there. A Linux CIFS mount exposes no ACL
through `flistxattr`, so its server-side Windows ACLs are not seen; the
user-facing documentation states this as well. A platform other than these
three has no known ACL interface, and its check fails rather than
trusting the mode bits. Refusing rather than reading ACLs is the
rule by construction: the three platforms' ACL semantics differ, an
allowlist of harmless entries is a list that grows with each platform, and a
user who wants an ACL on the destination can extract elsewhere. The rule
covers the fresh directory because a parent's inheritable entries attach to
it at creation; the check runs after `mkdirat`, and a failure removes the
directory while it is still the empty object just created and names the
parent. fsimg never strips or edits an ACL: that would add host-metadata
mutation to the extraction path for the sake of a parent the user can
replace. The checks run as the invoking user and hold against another
process with the same credentials only as far as the threat model states;
they are a safeguard against mistakes and the ordinary shared-directory
attacks, not an isolation boundary.

The same rule, by the same function, proves every other directory the CLI
coordinator creates before anything is put in it: the staging
directory of a created image, the staging directory of an `x --mtree`
manifest and the scratch tree. A directory created with mode 0700 can still
fail it: an inheritable entry of its parent can admit another user, a user
who can write a parent without the sticky bit can put another directory in
its place before it is reopened, and a filesystem without POSIX permissions,
such as FAT mounted on macOS, reports group and other write on everything.
Any of these could let another principal replace the directory or the staged
bytes before publication, so all of them refuse, and the error names the
parent; the user then places the output elsewhere, the extraction with
`-C` and scratch with `-T`, and copies the result. One creation routine makes and checks every such directory, so none
exists that skipped the check. A refused directory gives up its descriptor
at once, and with it all cleanup of its contents, which goes through that
descriptor: whatever the name now reaches may belong to someone else. It is
removed again only while the name still holds the object it opened, owned
by the user and empty; otherwise its name is reported as retained, unless
it now holds another object, which is not fsimg's to report. A writer of
the parent can still put another empty directory under the name between
that check and the removal, which gains nothing it could not do itself.

The selected parent comes from the user; image names never influence how
that initial authority is obtained. Merging into a populated tree is not
possible by any option.

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
critical entry kinds: an image entry of such a kind fails the extraction, or
is skipped and counted when the caller selects the explicit skip option of
the [POSIX metadata design](design-metadata.md#reading-extraction-and-display).
It does not restore owners, setuid/setgid bits, executable
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
for the supported uncompressed formats. A container's disk can be far longer
than its file (zero runs cost nothing), so the coordinator takes the disk
length from the worker's image record and cannot check it against the file:
for a container image the default bound rests on that worker-reported
length, and a caller who wants a bound independent of the worker sets
`-l output`. A caller can select another finite ceiling. Sparsity, cross-links, and repeated entry references do not evade it.
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
status is independent of worker success. With `cW` the staged image is first
verified by a reading worker; a failed verification discards the staged
result and publishes nothing, as the
[layer design](design-layers.md#create-then-verify) states. An image
streamed to stdout has no publication step: the staged result is transcoded
to the stream as the [layer design](design-layers.md#streams) specifies, and
a failure after the first forwarded byte is incomplete output, reported
through the exit status. Power loss may leave private staging;
stale staging cleanup must never follow symlinks or delete files by a broad
filename pattern.

## Other boundaries

Reopen host sources relative to authorized roots, without following symlinks,
and validate identities and lengths around copying. Output and scratch must
not be imported as sources. A source changed in place can defeat ordinary
timestamp-based detection; the source stability contract requires a snapshot
when an adversarial concurrent writer is relevant.

Decoders validate lengths, offsets, integer products, chain termination,
directory references, encoding, and required checksums before use. Partition
tables, container block tables, the DMG plist and compressed streams are
untrusted input under the same rules, decoded inside the worker; the
[layer design](design-layers.md#security) states their bounds and the spool
authority. In exFAT,
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
* Destination fixtures exercise both ways of obtaining the root, each passing
  case paired with a seeded failure that proves the check ran: a stem name
  already present as a file, an empty directory and a symbolic link to an
  outside directory (fails, the target untouched); `-E` on a symbolic link to
  an empty directory (fails on the no-follow open); `-E` on a directory with
  one entry, a dotfile, or an entry added between the emptiness check and
  the first creation (fails as a collision, the entry untouched); `-E` on a
  group-writable, other-writable or differently owned directory (fails) and
  an other-readable one (succeeds); a parent carrying a non-trivial
  inheritable ACL, or an `-E` directory carrying a non-trivial ACL, of each type the platform reports (FreeBSD POSIX.1e
  access, POSIX.1e default and NFSv4; Linux access, default and NFSv4
  attributes; macOS extended entries), including an entry that grants
  nothing, where the fresh case must remove its empty directory and name the
  parent; a trivial ACL that passes on FreeBSD and macOS, and on Linux a
  trivial POSIX ACL that leaves no attribute behind, while a listed
  `system.nfs4_acl` is refused; `-f -` and an underivable
  stem without `-E`; and a parent replaced by a symbolic link between the
  user's inspection and the open, which succeeds only into the link's target
  as a fresh directory and never into a populated one.
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
