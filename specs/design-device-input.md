# Read-only disk device inputs

## Scope and command behavior

An image input is a finite byte-addressable object or an explicit stream.
The named input of `i`, `t`, `W` and `x` accepts a regular file or a disk
device whose host interface supplies a finite capacity and positional reads.
For example, `fsimg i -f /dev/disk10` reads the required image metadata
directly. It does not copy the device into a temporary image first. Permission
to open the device comes from the invoking user's credentials; fsimg neither
elevates privileges nor mounts, unmounts, locks or changes the device.

The accepted device interfaces are:

| Host | Endpoint | Capacity and transfer quantum |
| --- | --- | --- |
| macOS | Disk block nodes such as `/dev/disk10`, and raw disk character nodes such as `/dev/rdisk10` | `DKIOCGETBLOCKCOUNT` multiplied, with overflow checking, by `DKIOCGETBLOCKSIZE` |
| Linux | Block devices, including disk and partition nodes | `BLKGETSIZE64` for bytes; `BLKSSZGET` for the logical sector size |
| FreeBSD | Disk character devices, including GEOM disk and partition providers | `DIOCGMEDIASIZE` for bytes; `DIOCGSECTORSIZE` for the transfer quantum |

The interfaces are defined by Apple's
[disk header](https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/sys/disk.h),
the Linux [block ioctl header](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/fs.h),
and FreeBSD's [disk header](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/sys/disk.h).
The host-specific descriptor class and successful geometry queries together
identify an accepted endpoint. A pathname prefix or a successful `lseek()`
does not. Linux character devices are outside this profile. On macOS and
FreeBSD a character device that does not implement the disk queries is
refused. There is no fallback from failed geometry discovery to reading until
EOF, probing for the last readable byte, or accepting a user-supplied size.

The opened endpoint defines byte zero and capacity. A whole-disk node exposes
the whole disk; a partition node exposes only that partition. fsimg does not
reopen the containing disk or switch between block and raw node names.
Partition selection runs on the bytes actually exposed by the endpoint. A
partition node containing a bare filesystem needs no outer partition selector.

`-f -` retains stream semantics even when stdin is a seekable file or a disk
device: the input relay feeds the worker's budgeted spool. A named FIFO,
socket, terminal, directory, or other non-disk special file is refused.
Callers can deliberately supply a finite stream on stdin; fsimg does not
guess stream semantics from a named operand. Final-component symlinks retain
the image-path no-follow rule, including convenient device-name symlinks.

This input support adds no writable device adapter. Image creation still
publishes a fresh regular file or writes an explicitly selected stream.
Source-tree scanning, manifests, layer file settings, scratch, spool and raw
intermediate descriptors retain their own regular-file requirements. A disk
node encountered beneath a source root is not an image-input operand.

## Owners and construction

The host compatibility layer discovers device geometry. Bounded I/O validates
that geometry, constructs the read-only object, and translates byte requests.
The CLI coordinator prepares the descriptor before worker confinement; the
worker consumes the prepared description without repeating discovery. Drivers
receive only ordinary bounded I/O objects.

One private input description is shared by CLI preparation, the worker
protocol, sandbox validation and image adoption. Its closed kind is absent,
regular file, disk device, or relay stream. For a positional input it carries
the declared byte length and transfer quantum; regular files use quantum 1.
Absent and stream descriptions carry no geometry. The wire codec encodes
these fields explicitly, validates their combinations, and rejects unknown
kinds and noncanonical unused fields. It does not serialize a native struct.
There is no separately mutable `is_device`, `is_seekable` or `needs_spool`
flag: the description decides transport, and the layer decoder decides
whether encoded contents need a spool.

The common preparation operation inspects the opened descriptor with
`fstat` and `F_GETFL`, requires `O_RDONLY`, and obtains geometry only for an
eligible device class. Device `st_size` has no length meaning here. The
capacity is positive, fits the host's positional-I/O range, and is a multiple
of the positive transfer quantum. The quantum fits `size_t`, `ssize_t` and
the capacity. Every multiplication, conversion and alignment calculation is
checked. These conditions do not require a power-of-two quantum; division
and remainder define alignment. An empty regular file retains length zero.

Device open uses the ordinary no-follow, close-on-exec, nonblocking and
no-controlling-terminal protections. They prevent a FIFO open from waiting
and a terminal from becoming controlling; they do not make every device open
harmless or nonblocking. Only the trusted operand selects this authority.
The device remains open from discovery through the operation. No path is
reopened after discovery, and the description cannot be replaced by worker
output. The coordinator derives its initial raw input/output bounds from
this capacity, never from device-node `st_size` or a worker-reported capacity.

### Library entry points and ownership

`fsimg_io_from_fd()` retains regular-file read/write access and accepts
the disk endpoints above only with `FSIMG_IO_READ` on an `O_RDONLY`
descriptor. It duplicates the caller's descriptor, prepares and validates
that duplicate for the requested access, and constructs the complete object
before returning it. Failure closes the duplicate and releases its charge;
the caller's descriptor remains open. Flags shared by the duplicate and the
original are neither changed nor permitted to change during the object's
lifetime. Unsupported direct-I/O flags remain refused. Device geometry
queries are an explicit consequence of this constructor; it neither enters
nor changes the application's sandbox.

The CLI uses a private image adopter that accepts the same prepared
description and an already charged descriptor. It consumes both ownership
and the descriptor reservation on success or failure. Before constructing
the object it rechecks descriptor class, access and geometry invariants,
without ioctl or image reads. For a regular file, the prepared length must
equal the size observed by `fstat`; a mismatch is `FSIMG_ESOURCE_CHANGED`.
For a device, `fstat` rechecks the class but cannot establish capacity, so
adoption validates the prepared geometry without querying the device again.
Only the trusted preparation path supplies geometry: no public constructor
accepts an unchecked capacity override.
An application already confined without discovery rights can prepare its
FD adapter before confinement or supply its own fixed-length positional
callbacks through `fsimg_io_from_callbacks()`.

Private constructors used for regular-file sources, file settings, output,
scratch and handed-over files remain regular-only. They share flag and
ownership helpers, but do not call an image constructor that broadens their
accepted types. The public constructor and private image adopter converge
on one complete root construction; callers do not patch its length or kind
afterwards. Slices borrow the root and share its descriptor and buffer.

The resulting device root offers positional reads and fixed size alone. It
has no write, resize, zeroing or durable-flush capability and cannot become
a fresh-file or scratch root. Its zero-range answer is always empty. It
never syncs a device, asks for `SEEK_HOLE`, or interprets unallocated storage
as zero. Ordinary filesystem-level initialized-content rules still apply
above it.

## Arbitrary byte reads over device sectors

Image codecs request byte ranges: a signature can be eight bytes and a
container footer can start inside a device sector. Requiring every caller
to align would spread host rules through parsers. The device adapter owns
the translation instead. Apple's
[disk read implementation](https://raw.githubusercontent.com/apple-oss-distributions/IOStorageFamily/main/IOMediaBSDClient.cpp)
requires raw character-device offsets and counts to align to media blocks;
FreeBSD's [GEOM device interface](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/geom/geom_dev.c)
likewise uses sector-aligned transfers. The same translation serves every
accepted device class.

Each device root owns one charged reusable transfer buffer, with no retained
read cache. Its size is the largest whole number of device sectors fitting
the ordinary 64 KiB transfer size, or one sector when that sector is larger.
The context must be able to reserve it before construction succeeds. This
keeps the cost independent of device capacity, adds no new user limit and
lets the existing memory budget refuse an impractical quantum. The supported
ordinary `pread` interfaces use ordinary allocator-aligned userspace buffers;
`O_DIRECT` and other caller-selected special transfer modes are not enabled.
Native qualification includes an unaligned caller buffer so this property
is tested, not inferred from sector-aligned offsets alone.

For a nonempty checked request at root offset `r`, with quantum `q`:

1. Compute `a = r - r % q`, the containing sector's start, and `d = r - a`.
2. Choose the largest requested prefix whose covering whole sectors fit the
   transfer buffer. Their length is positive, a multiple of `q`, and no
   greater than `capacity - a`. Round by checked division/remainder, never
   by an unchecked `r + count + q - 1` expression.
3. Make one positional read into the transfer buffer at `a`. Copy only the
   intersection of the confirmed host bytes with the requested prefix to
   the caller. The unused prefix and suffix are never exposed.

For example, bytes 510 through 513 on a 512-byte device require the two
sectors beginning at 0, but only four bytes reach the caller. At the last
byte of the device the covering sector ends exactly at capacity; the
adapter never attempts the next sector. A slice may require enclosing
sectors outside its logical range but inside the root device. Only its
requested bytes are returned. A slice is a parser boundary, not an OS
capability restricting a compromised worker to those sectors.

One adapter attempt performs at most one host read. A successful positive
short read ending on a sector boundary returns the confirmed requested
prefix; the common exact-read loop continues through the adapter, which
aligns the next request afresh. A host result ending inside a sector is an
I/O failure, with any confirmed requested prefix preserved. A zero-byte
host result for an in-capacity request is premature end and fails, even
for a non-exact read. Neither case invents zeros or copies stale buffer
contents. A zero-length request at logical EOF succeeds without a syscall;
an out-of-range request fails before translation.

The common loop owns work charging, cancellation checks, permitted EINTR
retries and the consecutive-interruption bound. Each host read costs one
adapter-call work unit, including retries, so alignment adds no hidden
retry loop. Confirmed-transfer results and `read_bytes` count the requested
bytes delivered, not the surrounding bytes fetched into the transfer buffer;
`io_calls` counts the attempts. These are library-I/O statistics, not a
measurement of hardware traffic. The buffer is released with the root on
every destruction and failed construction path.

## Layers and spooling

A named device enters the same positional image-open path as a regular
file. Compression and container probes still run; the endpoint kind does
not force a raw format. A raw device or a seekable container on it needs no
payload spool. gzip and a container profile with one sequential compressed
payload retain their explicit spool requirements and limits. A spare empty
spool descriptor prepared by the coordinator is not a copy of the device.
Metadata scratch remains available independently of payload spooling.

`--offset` and `--length` retain their meaning as a slice of the unwrapped
disk. They do not supply a missing device capacity or move the beginning
of compression/container probing. The same layer signatures, ambiguity
rules, bounded volume selection and decoded-output ceilings apply as for
file input.

### Raw device sectors and partition tables

The transfer quantum also identifies the raw endpoint's logical block size.
Partition LBAs use those units: the UEFI
[GPT and MBR specification](https://raw.githubusercontent.com/UEFI/UEFI-Specification-Release/main/source/05_GUID_Partition_Table_Format.rst)
defines table positions and partition lengths in logical blocks, and
Linux's [MBR reader](https://raw.githubusercontent.com/torvalds/linux/master/block/partitions/msdos.c)
scales its entries by the device's logical block size. Treating a native
4096-byte-sector MBR as 512-byte sectors would select the wrong bytes.

The I/O root's transfer quantum describes host reads. An I/O view's optional
logical sector size describes the bytes presented to partition discovery.
These are distinct facts: a fixed VHD can present a 512-byte-sector disk
while its backing device still requires 4096-byte reads. The view's fact
must survive selection and reopening, because an I/O object can outlive
the image-open call that established its representation.

Every I/O object holds an immutable logical sector size, with zero meaning
unknown. A device root initializes it from its transfer quantum; other
roots initialize it to unknown. The ordinary slice constructor copies the
immediate parent's value, even though byte translation flattens to the
storage root. Neither scheme opening nor slicing recovers this fact from
the storage root or a backing-identity link. No public setter changes it.

One common image-disk view constructor establishes the read-only view after
peeling and before scheme opening or returning a selection. It inherits the
input view's logical sector size when neither compression nor a container
was peeled, and initializes it to unknown when decoding established another
byte representation. That decision consumes the existing resolved layer
result at this single boundary. The result is an ordinary flattened view,
not another resolver or transfer buffer; it preserves backing identity,
zero-range behavior and byte translation. The image owns this view and
releases it before the input/container/spool storage it borrows. Ordinary
selection slices borrow it under the existing image lifetime contract.

Every container result crosses this boundary, including a fixed VHD's
zero-copy payload slice. Individual codecs and containers have no reset
operation or obligation, and no published object's fact is mutated. A
selected decoded disk, its nested slices and an image reopened over any
of them therefore keep unknown logical sector size without losing the
physical alignment enforced by their storage root.

Scheme selection keeps its signature and ambiguity rules. Before any
partitions are exposed, common scheme opening compares the scheme's chosen
LBA size with the disk view's known logical sector size and refuses a
mismatch with `FSIMG_EUNSUPPORTED`, naming both sizes. GPT supports its
specified 512- and 4096-byte profiles; an explicit `gpt:sector` does not
bypass the check.
The MBR profile uses 512-byte sectors and is therefore refused on a raw
endpoint with another logical block size. A partition node containing a
bare filesystem, or explicit byte-range selection, does not decode an
outer partition table and needs no such check. Filesystem sector sizes are
still validated from their own metadata; transport geometry does not
replace them.

## Confinement and blocking calls

Only the coordinator performs the fixed geometry queries, before spawning
the parser worker. It does not inspect image bytes. The worker receives the
read-only descriptor and the validated input description in its existing
startup protocol. Sandbox descriptor validation consumes that description:
a device is accepted only in the image-input role; a regular-file setting
or output cannot borrow that permission. Descriptor class and access are
checked before the existing readiness/acknowledgement boundary, and no
parser runs until confinement policy has been resolved.

Linux seccomp continues to deny every ioctl. FreeBSD grants the device
read, seek, fstat and the required flag-query rights, with an empty ioctl
allowlist and no sync or mutation right. Geometry changes are not handled
by granting the worker a query service. Neither platform opens another
device path for the worker. macOS retains its stated standalone policy:
`auto` reports unavailable confinement and `required` refuses to parse.
Read-only device support is not a new macOS sandbox backend.

The authorized readable object is the complete opened endpoint, including
unselected partitions and bytes outside allocated files. A compromised
worker with a whole-disk descriptor may read any of them. `--partition`
and byte-range selection limit correct parser operations, not descriptor
authority. A caller requiring narrower OS authority supplies a partition
device or a separately bounded image/provider.

One monotonic deadline still spans preparation and execution. The
coordinator checks cancellation and elapsed time before and after device
open and each geometry query, and does not start the worker after expiry.
Those calls are synchronous host calls: a driver can block despite
`O_NONBLOCK`, and the coordinator cannot guarantee a prompt return while
inside it. After startup the supervisor can request cancellation and
terminate the worker, but an uninterruptible kernel read may also delay
termination. The time limit is not a hard deadline on kernel or hardware
completion. Library calls have the same in-flight-call limitation and no
external supervisor. This contract adds no opener helper, retry process
or asynchronous device-I/O subsystem.

## Stability, aliases and failures

The capacity and quantum are fixed for the object's lifetime. fsimg does
not refresh them, reopen media after removal, or enlarge the input after a
device grows. A removed, shortened or failing device produces the host
error or a premature-end error. A same-sized replacement or concurrent
write can be invisible. No retry repairs damaged sectors, and no read
failure falls back to spooling or an unconfined worker.

Device-node timestamps, inode identity and `st_size` do not describe the
contents of the medium. A device root therefore supplies no regular-file
identity/change token to source checks; that absence is preserved through
slices and containers. It is not compared to a file size of zero, and its
node metadata is not presented as evidence of stable contents. Changes
actually detected by image validation or an existing multi-pass check
still fail normally. Successful verification says that the bytes observed
passed its checks, not that they formed one atomic snapshot.

The caller supplies stable media for the whole operation, including all
passes of a library wrap or compose. A mounted filesystem, another raw
writer or writable backing file can change those bytes. fsimg does not
enumerate mounts, infer whether a disk is idle or create a snapshot.
Unmounting alone does not exclude another writer. Snapshot consistency
requires an immutable image or an externally stable device/provider.

The caller also keeps writable output and scratch storage disjoint from
the input medium. A new output file on a filesystem mounted from the
input disk can modify that disk without any write through fsimg's input
descriptor. Device-node identities cannot establish physical disjointness
across partitions, raw/block aliases, loop devices, volume managers,
snapshots or mounted filesystems. The design does not build a storage
topology service. Regular-file alias checks remain in force; device-backed
inputs carry the same explicit disjoint-storage responsibility as opaque
callback providers. CLI and API documentation state this limitation.

| Failure | Result |
| --- | --- |
| Unsupported named object, device interface, or zero/inconsistent geometry | `FSIMG_EUNSUPPORTED`, naming the input and unsupported property |
| Descriptor flags incompatible with read-only adoption | `FSIMG_EINVAL`; no flag changes |
| Capacity multiplication, host range or conversion overflow | `FSIMG_ERANGE`, before image reads |
| Open/query/read failure, missing media, or premature end | `FSIMG_EIO`, retaining the OS error where present and the logical read offset where applicable |
| Buffer or descriptor budget exhausted | The existing precise resource error, before returning an I/O object |
| Cancellation or expired deadline | The existing cancellation/limit result; no successful completion or publication |

An unsupported ioctl is distinguished from a failed query on an otherwise
accepted device: the former identifies an unsupported interface, the
latter retains its I/O cause. Output retains the existing failure contract:
no staged image publication after failure; an incomplete extraction stays
private and is reported; a previously emitted stdout prefix is incomplete.

## Tests and acceptance evidence

Routine tests run unprivileged and write only disposable regular-file
fixtures. A private test seam substitutes the host geometry/read boundary
below the production device adapter; it does not make regular files devices
in the installed command or expose a public capacity override. The fake
host rejects unaligned requests and returns known bytes, so tests exercise
the actual translation rather than a separate callback implementation.

Required cases include:

* Geometry with 512- and 4096-byte sectors, a non-power-of-two quantum,
  zero and inconsistent values, capacity multiplication overflow, the host
  offset ceiling, and a quantum exceeding the memory budget. Device
  `st_size` is zero or misleading in these fixtures.
* One-byte, cross-sector, multi-buffer and final-byte reads; nested slices;
  an unaligned destination buffer; a whole-device EOF; out-of-range reads
  that invoke no host I/O. Poison the transfer buffer before short reads.
* Sector-aligned short reads, a short result inside a sector, zero progress,
  EINTR exhaustion, cancellation between attempts, I/O failure after a
  confirmed prefix, media removal and size changes. Check bytes, error
  offsets, work charges and allocation/descriptor cleanup.
* A huge synthetic device whose `i` result needs bounded metadata reads
  and no payload spool, using a spool limit smaller than the device;
  ordinary regular-file inputs produce the same image results. A raw
  device succeeds with `-T -` when its metadata fits memory; gzip on a
  device retains the spool refusal and accounting rules.
* Malformed startup kinds/geometry, wrong descriptor class/access for a
  role, and forged worker capacity reports. Device descriptors are refused
  as source files, CLI layer settings, output, spool and raw intermediate.
  Write/resize/zero/durable-flush requests on a device adapter never issue
  a mutating syscall. Zero-range queries issue neither sync nor hole query.
* Partition endpoints and whole disks, image layers with tail probes,
  ordinary extraction/verification, failure before publication, and the
  explicit stream path's unchanged spool behavior. Two library device
  adapters share the context budget; slices do not duplicate their buffers.
  Raw GPT geometry must match its endpoint; raw MBR on a 4096-byte endpoint
  is refused. A container holding a 512-byte-sector disk on that same
  endpoint remains readable, including a fixed VHD whose payload is a
  zero-copy slice, proving that decoded disk geometry is separate.

The representation regression follows the public API: open a fixed VHD
containing a 512-byte-sector MBR disk through a synthetic 4096-byte device,
select `FSIMG_SELECT_DISK`, and pass that returned I/O object to
`fsimg_image_open()`. Partition discovery must yield the same byte offsets
as the first image. Repeat through nested `fsimg_io_slice()` views and
another select/reopen cycle. In the same fixture, an ordinary slice of a
raw 4096-byte device retains its known sector size and refuses the 512-byte
MBR profile. Record host requests to prove all decoded-view reads still
obey the root's 4096-byte transfer alignment. These cases distinguish
correct representation inheritance from simply disabling sector checks.

Mocked descriptors do not qualify host device support. Separate opt-in
platform tests provision disposable virtual media: read-only attached disk
images on macOS, read-only loop devices on Linux, and read-only md/GEOM
providers on FreeBSD. Provisioning may require privileges; the test command
itself runs with only permission to read its endpoint. No test opens an
operator's physical disk. The fixture owner detaches the media on every
exit path. Record OS/kernel, endpoint class, capacity, quantum, commands
and results; a missing device facility or privilege is an explicit skip.

macOS tests both block and raw character endpoints. Linux and FreeBSD run
the reads under required confinement and verify that geometry and mutating
ioctls, device writes and unrelated opens are denied after setup. The same
fixtures exercise raw filesystems, GPT whole disks, partition endpoints and
512/4096-byte transfers, with independently prepared filesystem contents.
Media-removal tests use only disposable providers. ASan/UBSan cover buffers,
ownership and arithmetic; LSan/MSan run on supported Linux configurations.
All three platform results and skips accompany the implementation review.

## Design tradeoffs

The added concepts are a prepared image-input description and one read-only
device root with a bounded transfer buffer. They replace repeated
regular-file admission/size decisions in the CLI and worker with one
preparation boundary. The protocol, sandbox and I/O layer consume the same
description; format drivers gain no host behavior. Public FD use and CLI
adoption share validation and translation rather than maintaining separate
device implementations.

Sector provenance is necessary because interpreting a raw 4096-byte-sector
MBR with 512-byte units selects the wrong storage. Removing that check would
hide the error; refusing device-backed containers or reopened selections
would discard useful positional-I/O behavior. The representation costs one
immutable scalar per I/O object and one ordinary disk view per image open.
It replaces a rule based on the current image-open history with a fact the
returned object preserves. The existing slice constructor owns inheritance,
and the common decoded-view construction owns the representation change.
There is no mutable reset state, additional resolver depth, new descriptor
owner, or list of participating codecs for a maintainer to remember.

A stream fallback would turn a small metadata query into a capacity-sized
copy. A generic nonregular-file adapter would promise seek, size and EOF
semantics that many devices do not have. Worker geometry queries would add
ioctl authority merely to rediscover facts the trusted opener can supply.
A sector cache or direct-to-caller fast path adds state without being
needed for bounded correct reads; one reusable buffer has one ownership
rule. Mount/topology discovery and hard-deadline opener processes address
broader contracts and are not prerequisites for reading a caller-selected
stable disk.
