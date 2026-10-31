# Extraction and worker execution

## Safe host extraction

The extractor consumes a reader before its first iteration and creates one
fresh private destination beneath a trusted parent descriptor. It accepts a
single destination basename. Existing files, directories and symlinks fail.
The internal worker entry point instead borrows a root descriptor from its
coordinator; it never receives the final parent authority. The coordinator
obtains that root in one of two ways defined by the
[security specification](design-security.md#safe-extraction): it creates
the directory exclusively inside the `-C` parent, named after the image
stem, or with `-E` it proves the user's own `-C` directory empty and private
on its opened descriptor. The worker cannot tell the two apart, and does not
need to: a verified empty private directory is the state exclusive creation
produces, and everything beneath it is created exclusively and no-follow.

The common reader selector resolves requested components through each format's
validated name/alias namespace and prunes unrelated directory branches. Its
bounded requested/resolved path buffers belong to the resource context. It
returns ancestors, the selected entry, and selected descendants as distinct
relations. Listing can limit descendants to immediate children; `x -O` requires
the selected entry to be a regular file or a symbolic link, whose bytes are
its target, as the [CLI design](design-cli.md#extraction) defines the mode.
All host paths use displayed names.

Every image path is independently validated at the extraction boundary. A
component-aware selection includes the selected entry/subtree and its ancestor
directories, preserving paths below the image root. A missing selection fails.
The reader visits directories before their children, depth first. The extractor
uses that order to keep a bounded stack of opened output directories and their
exact image path prefixes. Every selected directory is created exactly once;
there is no existing-directory merge case. Exclusive creation rejects host
case/normalization collisions as well as image duplicates. Each child open is
relative to its verified parent descriptor with no-follow protection.

The stack, path buffer and transfer buffer use the reader's resource context.
Depth, descriptor, path, memory and work limits remain independent. Entire
regular-file logical lengths, including zero ranges, are charged before file
creation or extension. Fresh files can preserve holes through the common
known-zero I/O boundary. Data ranges use bounded positional copies, then file
flush and checked close complete the file. No extraction callback resets the
reader's session quotas.

The default profile requests mode 0700 directories and mode 0600 regular files;
the caller's umask can narrow those modes. It restores no host metadata. Safe timestamp preservation is an
explicit in-process option; it supports access/modification times through open
file descriptors. Wall times require an explicit interpretation offset.
Directory timestamps are applied after descendants finish. Creation times and
attributes that are omitted follow the explicit allow-loss policy; required
losses fail. Structured callbacks and aggregate loss statistics expose losses.
No option enables links, privileged metadata, existing-tree merging or process
sandbox transitions. Failure leaves the destination private and identifies its
incomplete state; cleanup never recursively walks image-derived paths.

## Process boundary and protocol

The CLI coordinator opens trusted operands and prepares scratch/output authority.
Output and scratch use fresh disjoint private trees; neither contains the other.
Workers can delete scratch metadata, while output cleanup belongs to the
coordinator after the worker exits.
It starts its own executable at an absolute path with a minimal environment.
Spawn file actions preserve only designated descriptors, redirect standard
streams, and close all other handles. The worker disables core dumps before
installing confinement. No image bytes or source names are read during setup.
The command links the core objects directly, so fresh worker execution does not
depend on a dynamic-loader search-path environment. The installed library is
built from the same object collection and retains independent static/shared use.

A bounded versioned protocol carries readiness, acknowledgement, operation
records, errors and completion. Its header states magic, version, type and
payload length in explicit byte order. Lengths and message order are checked
before payload allocation or interpretation. Entry names use start/chunk/end
frames with a checked total path limit. The same entry records carry a
listing's entries, the one selected entry of a raw member output and the
names a verbose creation or extraction reports from the library's
[entry callback](design-api.md#options-context-and-callbacks); the worker
sends each as the callback fires, and a failed send puts the channel in the
failed state that cancels the job at its next poll, as a progress or loss
frame's does. The coordinator admits entry records by job and state: a
listing or member output in its output-ready state, a verbose creation
before its planned-disk and plan records, a verbose extraction after its
volume record, and none in any other job or state, a creation or extraction
not asked for names included. A sequence completes before any other frame,
each name counts against the entries limit and each path against the path
limit, and the counts must agree: a listing's completion count equals its
entries, a creation's plan count equals the names before it, and an
extraction's completion counts equal the names by kind, regular files and
directories against completed entries and every other kind against skipped
entries. Raw `x -O` bytes use a separate data pipe,
and a streamed image input arrives through its mirror, the input pipe the
coordinator fills from standard input without reading it, as the
[CLI design](design-cli.md#coordinator-and-streaming-representation) states.
No protocol message grants a pathname or descriptor-opening service.

The protocol header owns named field offsets shared by encoders, validators,
and formatters. Validators check typed fields before the supervisor advances
message order. Loss frames provide bounded examples (at most 32 per operation
with 256-byte path excerpts); completion carries the full aggregate count.

The worker first reports active, unavailable or disabled confinement. Automatic
mode warns about unavailable confinement before sending acknowledgement;
required mode refuses to acknowledge an unavailable profile. Unexpected or
partial setup failures always fail. Off mode is explicitly reported. The
dedicated cancellation pipe remains open for the operation: EOF or a cancellation
byte cancels the worker at its next I/O boundary, including after coordinator
death. In-flight host calls retain their OS completion/interruption behavior.
The coordinator supervises signals
and deadlines and requires legal completion plus successful child exit. Output
publication additionally requires the staged regular-file type and planned size
and runs only after all writable worker descriptors are closed.

The protocol and supervisor are independent of command formatting. The worker
can perform source planning/emission or a reader job: info, list, raw member output, verify or extract;
all detection and parsing occur after acknowledgement. The coordinator handles
terminal escaping and streaming machine records. Worker process separation on a
platform without a supported sandbox is reported as unconfined.

An output relay runs from the same executable with only its input/status pipes
and borrowed stdout/stderr sinks. The coordinator forwards raw `x -O` bytes only
after the complete selected-entry record is validated, and enforces the declared
file length and output limit before forwarding each bounded batch. Formatting
callbacks return bounded text spans to this same owner; they never write sinks.
Confinement warnings are confirmed on stderr before parsing is acknowledged.
Its own pipe writes are nonblocking; potentially blocking sink writes belong
only to the relay. Cancellation terminates both children without changing the
borrowed sinks' flags. Success requires both successful exits, complete output
streams, and the relay's exact final confirmed byte counts. Confirmed progress on
failure is a lower bound: termination can occur between a sink write and its
acknowledgement. Any forwarded prefix makes failed raw output incomplete.
