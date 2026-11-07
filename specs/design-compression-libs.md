# Optional compression libraries: zstd and xz

fsimg reads and writes gzip-compressed images with the system zlib, which the
core requires. zstd and xz are the other two whole-file compressions disk
images travel in: `zstd(1)` and `xz(1)` output, `.img.zst` and `.img.xz`
release artefacts, and the `zstd` fixtures the project's own large-image
checks use. This design adds both as **optional integrations** over the
system `libzstd` and `liblzma`: a build that has a library reads and writes
its compression with the gzip profile's shape; a build that lacks it is the
core as it stands, and refuses the compression exactly as it does today.
The same two libraries decode the MicroLZMA and Zstandard clusters of EROFS
volumes, [below](#erofs-block-decoding).

The [architecture](architecture.md#dependencies-and-implementation-organization)
owns the dependency rule and the optional-integration mechanics this design
instantiates. The [layer design](design-layers.md) owns compression's place
in the stack, the [spool](design-layers.md#stream-input-and-the-spool), the
[transcode](design-layers.md#transcode) and the gzip profile this design
mirrors; the [binary primitives](design-binary-primitives.md) own the
context-backed allocation adapter; the
[security design](design-security.md#coordinator-and-worker) owns the worker
the decoders run in; the [settings catalogue](design-settings.md) owns the
keys.

## Problem and evidence

* **Formats.** zstd is RFC 8878: a file is a sequence of frames, each a
  magic number `28 B5 2F FD`, a frame header of 2 to 14 further bytes
  (descriptor, window descriptor, optional dictionary ID, optional content
  size), blocks of at most 128 KiB each with a 3-byte header, and an
  optional 4-byte content checksum (the low 32 bits of XXH64). Skippable
  frames (magic `50 2A 4D 18` to `5F 2A 4D 18`, a 4-byte length, the
  bytes) may appear between frames and carry no content. xz is the
  `.xz` file format, version 1.2.1: a file is one or more streams, each a
  12-byte header (magic `FD 37 7A 58 5A 00`, two flag bytes naming the check
  type, CRC32), blocks (a header of at most 1024 bytes, LZMA2 data, padding,
  the check), an index of the blocks' sizes with its CRC32, and a 12-byte
  footer tying back to the index; streams may be separated by **stream
  padding**, zero bytes in multiples of four. The check is none, CRC32,
  CRC64 or SHA-256; eleven other IDs are reserved with defined sizes.
* **Tools.** `zstd -d` and `xz -d` decode concatenated frames and streams,
  skip skippable frames and stream padding, and fail on any other trailing
  byte. `zstd` writes a content checksum by default and a content size when
  it knows the input length; `xz` writes CRC64 by default. Readers in boot
  loaders and kernels decode xz only with CRC32 or no check, which is why
  `xz --check=crc32` exists.
* **Libraries.** `libzstd` is BSD-3-Clause or GPL-2.0 at the user's choice;
  `liblzma` is 0BSD. FreeBSD's base system ships `liblzma` with its
  pkg-config file; its `libzstd` is private to the base system
  (`libprivatezstd`, no public header), so FreeBSD takes `libzstd` from the
  `archivers/zstd` port. Both are standard packages on Linux (`libzstd-dev`, `liblzma-dev`; `libzstd-devel`,
  `xz-devel`) and Homebrew formulas on macOS (`zstd`, `xz`), each with a
  pkg-config file (`libzstd`, `liblzma`). On the development host:
  libzstd 1.5.7, liblzma 5.8.4, zlib 1.2.12. Neither ships headers in the
  macOS base system, so a macOS build without Homebrew is a build without
  them, which is why both are optional and why `fsimg --formats` has to say
  which the running build has.
* **Memory.** The libraries' memory is dominated by the window. zstd
  decodes with the frame's window, 8 MiB at `zstd -19` and up to 128 MiB
  under `--ultra`; its streaming decoder refuses larger windows by default
  (`ZSTD_WINDOWLOG_LIMIT_DEFAULT` is 27). The xz decoder needs the
  dictionary the stream declares, 8 MiB at `xz -6` and 64 MiB at `xz -9`,
  and LZMA2 allows up to 4 GiB; the xz **encoder** needs 94 MiB at preset 6
  and 674 MiB at preset 9 (`xz(1)`), which the default 256 MiB memory
  budget does not hold.
* **API facts that shape the design.** `liblzma` takes a custom allocator
  through the stable `lzma_allocator` record. `libzstd` exposes its custom
  allocator (`ZSTD_customMem`, `ZSTD_create*Stream_advanced`) and its
  frame header parser (`ZSTD_getFrameHeader`) only in the
  `ZSTD_STATIC_LINKING_ONLY` section of `zstd.h`, which the library
  documents as not covered by its ABI stability promise; the stable API
  offers `ZSTD_d_windowLogMax` to cap the window and `ZSTD_sizeof_DStream`
  and `ZSTD_sizeof_CStream` to read a context's current memory use. The
  zstd binding uses both, behind a configure link test that refuses a
  library without them; the stable-API alternative is under
  [alternatives considered](#alternatives-considered).
* **Sandbox.** The decoders run inside the confined worker. The Linux
  profile allows `read`, `write`, `pread64`, `pwrite64`, `close`, `fstat`,
  `lseek`, `fsync`, `ftruncate`, `mkdirat`, `unlinkat`, `getdents64`,
  `brk`, `mmap`, `munmap`, `mremap`, `mprotect`, `madvise`, `futex`,
  `clock_gettime`, the signal calls, `exit`, `fcntl(F_GETFL)` and a
  Landlock-mediated `openat` without `O_PATH`; everything else fails with
  `EPERM`, and `clone` among it, so a library that started a thread would
  fail loudly, not silently. Both libraries are pure computation over
  caller buffers plus `malloc`: with `ZSTD_c_nbWorkers` at its default of
  zero and `lzma_easy_encoder`/`lzma_stream_decoder` (not the `_mt`
  variants) neither creates a thread, opens a file or queries the host.
  liblzma's CPU-feature detection for CRC acceleration runs in a load-time
  resolver on x86-64 and through `getauxval` or `elf_aux_info` on arm64,
  both in user space; `lzma_physmem` and `lzma_cputhreads`, the two
  routines that do ask the kernel, are never called.

## Foundations

The design reuses mechanisms the core already has:

* `lib/layers/stack.c` is the single source of layer names and suffixes,
  so a registered compression's suffix reaches the three CLI uses (suffix
  stripping for an extraction stem, `-a` selection and the
  container-suffix conflict check) whatever operations its row carries.
  `fsimg_compression_named()` turns a registered compression without the
  needed operation into `FSIMG_EUNSUPPORTED` naming it, for a write
  (`compression` in the write options) and a read (the image options'
  forced compression) alike.
* `cmd/fsimg/options.c` refuses a creation whose selected compression lacks
  `FSIMG_CAP_WRITE` with "writing this compression is not supported" and
  exit status 1, after the rest of the command line is checked.
* `configure.ac` finds zlib through pkg-config or `zlib.h` and `-lz`,
  accepts `ZLIB_CFLAGS` and `ZLIB_LIBS`, fails without it, and puts
  `ZLIB_LIBS` into `libfsimg.pc`'s `Libs.private`.
* `lib/layers/gzip.c` is the profile this design mirrors: a probe on the
  prefix, a decoder that reads the source through `fsimg_spool_input_t`
  in 64 KiB pieces and appends decoded bytes to the spool in 256 KiB steps,
  each reserved before it is written and charged as work, with members
  checked as they end; and an encoder that is a sequential sink over the
  transcode's next sink, deflating in 64 KiB pieces with a bound the plan
  charges to the output limit.

## Design

### Central construction

A compression is implemented when, and only when, its registry row carries
operations. Every other behaviour is already a function of the registry:

| Behaviour | Derives from |
| --- | --- |
| Suffix stripping, `-a` selection, container-suffix conflict | The row's `suffix`, present in every build |
| Refusal without the library | `fsimg_compression_named()`: operations absent is `FSIMG_EUNSUPPORTED` naming the compression; the CLI's `FSIMG_CAP_WRITE` check gives the same answer before the worker starts |
| Detection by magic, forcing, ambiguity | The step-1 survey runs every registered compression's `probe`; a row without one is not surveyed |
| Reading into the spool, limits, work, cancellation | The compression step calls the row's `decode` with the spool and its input |
| Writing, bounds, streams, create-then-verify | The disk plan calls `plan` and `bound`; the transcode wraps its sink in `encoder` |
| Settings keys, validation, `--formats` listing | The row's settings slice |
| Codec version in `--formats` and the plan report | The row's `version` operation |
| Verification scope | The spool-time checks `decode` performs, and the row's one `unverified` entry, which verification reports when `decode` says a member lacked that check |
| EROFS cluster decoding | The row's `block_decode`, present when the library and its block interface are found ([below](#erofs-block-decoding)) |

So the design has one conditional per library, evaluated once at configure
time: the library is present, so `lib/layers/zstd.c` (or `xz.c`) is
compiled and the row names its operations; or it is absent, so the row is
the reserved row the code has today. Where one operation needs an interface
newer than the library's baseline, as `block_decode` needs liblzma 5.4,
configure probes that interface too and the file leaves that one operation
NULL without it, refused as unsupported naming the library and version
([architecture](architecture.md#dependencies-and-implementation-organization)).
No other `#if` exists outside the one source file and the registry row.
There is no plugin, no second registry, no runtime probing for a library,
and the public header is the same in every build.

Two generalizations make the existing mechanisms carry three libraries
without a list of cases:

* **One allocation adapter.** The zlib adapter's state, overflow check,
  first-error rule and `fsimg_alloc`/`fsimg_dealloc` calls are library
  neutral; only the callback signatures are zlib's. The adapter becomes
  `fsimg_codec_memory_t` in `lib/core/codec-memory.c` with two neutral
  entry points, `fsimg_codec_memory_alloc(memory, items, size)` and
  `fsimg_codec_memory_free(address)`, and each library's shims live in the
  file that includes that library's header: `fsimg_zlib_memory_bind()` in
  the core (zlib is always present), `lzma_bind()` in `xz.c` filling an
  `lzma_allocator`, and `zstd_bind()` in `zstd.c` filling a `ZSTD_customMem`. The rules of the
  [binary primitives](design-binary-primitives.md#context-backed-zlib-allocation)
  apply to every binding unchanged: first error kept, zero-byte requests
  charged, no second budget.
* **A version operation instead of a flag.** The `bool zlib` on
  `fsimg_compression_ops_t` and `fsimg_container_ops_t` becomes
  `void (*version)(char *, size_t)`, which writes the library name and
  runtime version (`"zlib 1.3.1"`, `"zstd 1.5.7"`, `"liblzma 5.8.4"`), or
  is NULL for a layer whose bytes depend on no library. `fsimg_layer_info`
  and `fsimg_disk_plan_codec()` call it; the enumeration of libraries that
  the two flags were disappears. `ZSTD_versionString()` and
  `lzma_version_string()` are the runtime values, as `zlibVersion()` is,
  so a shared library newer than the build's header is reported as the
  version that actually produced the bytes.

### Packaging and build

`configure` gains two options with the same three states:

```text
--with-zstd[=auto|yes|no]   libzstd for the zstd compression (default auto)
--with-xz[=auto|yes|no]     liblzma for the xz compression (default auto)
```

Detection mirrors zlib's: pkg-config (`libzstd`, `liblzma`) supplies the
flags when it describes the library, else the header and `-lzstd` or
`-llzma` are tried on the default paths; `ZSTD_CFLAGS`, `ZSTD_LIBS`,
`LZMA_CFLAGS` and `LZMA_LIBS` override both and are precious, so
`sanitize.sh` replays them. The link test compiles a program that calls the
exact functions the profile uses (`ZSTD_createDStream_advanced`,
`ZSTD_createCStream_advanced`, `ZSTD_getFrameHeader`, the parameter
setters, the streaming calls and `ZSTD_versionString`;
`lzma_stream_decoder`, `lzma_easy_encoder` and `lzma_version_string`), so
a library too old or built without the needed interface counts as absent.
A second liblzma test, for the MicroLZMA decoder EROFS needs, follows it
and disables only that decoder ([below](#erofs-block-decoding)).
`yes` fails configure naming the library when the test fails; `auto`
disables the compression and says so in configure's output; `no` never
looks. The result is an Automake conditional (`FSIMG_ZSTD`, `FSIMG_XZ`), a
`config.h` macro of the same name, and the libraries' flags in
`AM_CPPFLAGS`, `libfsimg_core_la_LIBADD` and `libfsimg.pc`'s
`Libs.private`, beside zlib's. The installed-library checks link with
`pkg-config --static --libs libfsimg` instead of repeating `$(ZLIB_LIBS)`,
so they cannot drift from the metadata.

The optional sources are compiled only under their conditional and
distributed in every build: Automake distributes the sources of every
branch of a conditional, and `check-dist.sh` fails a release that drops a
tracked file. The
public header `include/fsimg.h` does not change between builds: the layer
option fields and enums below exist always, the catalogue rows exist
always, and only the registry row's operations differ, so an embedding
application compiled against one build runs against another and learns
the difference from `fsimg_layer_info_t.capabilities`, never from a
missing symbol or a different structure layout.

The MemorySanitizer build cannot run uninstrumented libraries, so
`sanitize.sh msan` configures `--without-zstd --without-xz` and its report
names both paths as not covered by MSan; ASan, UBSan, LSan and the fuzz
build run with the libraries the outer build found. `build-aux/linux-check`
installs `libzstd-dev`, `liblzma-dev` and `xz-utils` beside `zstd`.

### Capability reporting

`fsimg --formats` lists each compression as it lists gzip: name, kind,
read/write/verify flags, suffix, settings, and on the second line the
library version from the row's `version` operation. In a build without
the library the row shows `no` in every support column, its suffix and its
settings, and no version: the honest statement that the name and suffix
are recognized and nothing can be done with them. The JSON `format` record
is unchanged in shape; `capabilities` is 0 and `codec_version` null for an
unimplemented row. `i` reports the detected compression beside the stack
(`Compression:  zstd`), and the plan report's `codec_version` names the
libraries an output depends on: the container's when its resolved
encoding uses one, as DMG's zlib chunks do, then the compression's, each
once and separated by `, ` (`zlib 1.3.1, liblzma 5.8.4` for a `.dmg.xz`).
Runtime sandbox reporting is unchanged.

The layer report names what a decode left unchecked. A compression's
operation record states the one entry it can leave, `zstd:checksum` or
`xz:check`, qualified by the compression's name; `decode` reports whether
it decoded any frame or stream without that check, and verification then
adds the entry to the `unverified` list. A container's entries are its
own unqualified field names (`0x14,0x1c`), so `W`'s human line attributes
each entry to the compression or the container by that qualification.

### Profiles

Both profiles have the gzip profile's shape and share its rules without
restating them: the probe claims on identity from the prefix; `decode`
reads the source once through `fsimg_spool_input_t`, charges every
compressed byte it reads and every decoded byte it produces to the work
counter, appends to the spool in steps of at most 256 KiB, each reserved
against the spool limit before it is written, so a bomb fails at the step
that would cross the limit with `FSIMG_RESOURCE_SPOOL`; the decoder's
state and buffers are charged to memory through the adapter; cancellation
is polled at every input read; the encoder is a sequential sink over the
next one, fed in 64 KiB pieces with a zero run encoded from a buffer of
zeros, with its state charged to memory; the plan charges `bound` to the
output limit and the sink refuses a byte past it as an internal error.
Compression is peeled once at the file level: a spool whose content begins
with another compression's signature is an unrecognized image. The
[spool-backed-peel rule](design-layers.md#spool-backed-peel) applies: an
`hbcifs` container inside zstd or xz is refused naming both layers, as it
is inside gzip.

#### zstd

Reference: RFC 8878. Oracles: `zstd -t`, `zstd -l`, `zstd -d`, and files
produced by `zstd` at several levels, with `--no-check`, with
`--content-size` absent (a pipe), with `--ultra -22`, as concatenated
frames (`cat a.zst b.zst`), and with a skippable frame.

Identity: the four magic bytes `28 B5 2F FD`, or a skippable magic
`5x 2A 4D 18`. A skippable first frame is a claim, since `zstd -d` reads
such a file; what follows it must be a frame. An assumed identity
(`--force`) waives nothing: libzstd reads the magic itself, so a frame
whose magic is damaged stays unreadable.

Writing: one frame from `ZSTD_compressStream2` with `ZSTD_e_continue` per
piece and one `ZSTD_e_end`, on a context whose parameters are set
explicitly rather than left to the library's defaults, so the frame's
shape is fsimg's: `ZSTD_c_compressionLevel` from `zstd:level`;
`ZSTD_c_checksumFlag` 1, so every frame fsimg writes carries a content
checksum, as `zstd(1)` writes by default; `ZSTD_c_contentSizeFlag` 1 with
`ZSTD_CCtx_setPledgedSrcSize` set to the length the plan knows the
compression's input to have, so the frame header declares the content
size, `zstd -l` shows it and a reader can tie the decoded length to it;
`ZSTD_c_nbWorkers` 0. The input length is known exactly when the
compression wraps the bare disk, whose length the layout fixes; a
container's encoding is bounded but not known in advance, so a zstd frame
around a container declares no content size, as `zstd(1)` writing from a
pipe does, and still carries its checksum. The plan records the length in
the compression settings, unknown for a container. The window
is the level's own, at most 8 MiB at level 19. The library checks a
pledged size at `ZSTD_e_end`, so a transcode that delivers a different
length than it planned fails there as `FSIMG_ESTATE`, the same class as a
sink receiving a byte past its bound. The output's bound is
`n + n/256 + 534` for `n` input bytes, with each division rounding down:
the frame header (at most 18 bytes), the checksum (4), and `n + n/256 +
512`, which exceeds `ZSTD_compressBound()` for every `n`. The format
itself bounds nothing, since a frame may hold any number of empty blocks;
the bound rests on libzstd's encoder policy with no intermediate flush,
which stores incompressible input as raw blocks of 128 KiB, each behind a
3-byte header. A library that broke it fails as `FSIMG_ESTATE` when the
sink refuses the byte past the bound, never as a short output.

Reading: a `ZSTD_DStream` with `ZSTD_d_windowLogMax` set explicitly to 27
(128 MiB, the library's own default made fsimg's), decoding frames in
sequence as `zstd -d` does; the library consumes skippable frames and
produces nothing from them. At each frame boundary fsimg parses the frame's
header with `ZSTD_getFrameHeader()` from its input buffer, which it tops up
to at least the 18-byte maximum header or the rest of the input before
asking: the header's dictionary ID and window size are refused, as below,
before the library allocates a window; its checksum flag says whether the
frame is checked; and at the frame's end the decoded length must equal a
declared content size. The library checks the content checksum when the
frame carries one. A frame without a content checksum decodes, and the decoder
records `zstd:checksum` in the layer report's `unverified` list, so the
[verification scope](#verification-scope) is honest about what was
checked. A frame whose window exceeds the limit is
`FSIMG_EUNSUPPORTED` naming the limit; a frame that names a dictionary is
`FSIMG_EUNSUPPORTED`, since fsimg has none; bytes after the last frame
that are not a frame are `FSIMG_ECORRUPT` as trailing bytes; input that
ends inside a frame is `FSIMG_ECORRUPT` as truncated. A header the parser
refuses for another reason, such as a window beyond the format's or a
reserved bit, maps through the error table below as the decoder's own
result does. Every corrupt result names the compressed offset where the
failing call's input began, which is at or before the point where the
library detected the failure: a failed call does not report how far it
read, and a damaged checksum or content size is detected only at the
frame's end.

The header parse is also what keeps a zstd decode inside the memory
budget. libzstd's stream decoder also decodes legacy frames (formats
v0.5 to v0.7), whose decoders allocate with `malloc` outside the custom
allocator. `ZSTD_getFrameHeader()` accepts only current and skippable
frames, and the stream decoder stops at the end of every frame, so every
frame start passes the parse before the library sees it: a legacy frame,
first or after another, is trailing bytes, never decoded.

Seekable zstd (the skippable seek table of the `zstd` contrib format)
decodes through the spool like any other zstd file, because the seek table
is a skippable frame. Indexing it to read in place without a spool is a
later profile, not part of this design.

#### xz

Reference: the `.xz` file format specification, version 1.2.1. Oracles:
`xz -t`, `xz -l`, `xz -d`, and files produced by `xz` at several presets,
with each `--check` value, as concatenated streams with and without stream
padding, and with `-T0` (several blocks in one stream).

Identity: the six magic bytes `FD 37 7A 58 5A 00`. The stream flags and
header CRC32 are checked after the claim, by the library, and a failure
there is `FSIMG_ECORRUPT`, never a fall-through. An assumed identity
(`--force`) waives nothing: liblzma reads the magic itself, so a stream
whose magic is damaged stays unreadable.

Writing: one stream of one block from `lzma_easy_encoder` with the preset
`xz:level` names and the check `xz:check` names, fed with `LZMA_RUN` per
piece and one `LZMA_FINISH`; no `LZMA_PRESET_EXTREME`, no filter chain
beyond LZMA2, no multithreaded encoder. The output's bound is
`n + n/4096 + 2048`. The streaming LZMA2 encoder ends a chunk when its
compressed data nears 64 KiB or its input reaches 2 MiB, and stores a
chunk that did not shrink as uncompressed data, so no chunk is more than
its six header bytes longer than the input it holds, and each holds about
64 KiB or more: `n/4096` allows sixteen bytes per 64 KiB. (The bound of
uncompressed chunks alone, 3 bytes per 64 KiB, is not enough: a chunk that
shrank by one byte keeps its compressed form and its longer header.) The
stream header and footer (24), the block header (at most 1024), the block
padding (at most 3), the check (at most 64), and the one-record index with
its padding and CRC32 (fewer than 64) fit the constant.
`lzma_stream_buffer_bound()` and the encoder's real output on
incompressible and barely compressible input are the oracles the tests
compare it against, never the value the plan uses, so the bound rests on
LZMA2's chunking rather than on a library version; a liblzma that broke it
would fail the write as `FSIMG_ESTATE` when the sink refused the byte past
the bound, never write past it. The encoder's memory
is the preset's, up to 674 MiB at preset 9; an encoder the memory budget
cannot hold fails at `encoder_open` with the adapter's `FSIMG_RESOURCE_MEMORY`
before any byte is written, and the man page states the per-preset need
from `xz(1)` so the user can raise `-l memory=` or lower the preset.

Reading: `lzma_stream_decoder` with the flags `LZMA_CONCATENATED`,
`LZMA_TELL_NO_CHECK` and `LZMA_TELL_UNSUPPORTED_CHECK` and no memory
limit of its own (`UINT64_MAX`): every allocation, the dictionary
included, goes through the adapter and is charged to the context, so a
declared 4 GiB dictionary fails as a memory limit at the allocation and a
second bound would only restate the first. Streams and stream padding are
decoded in sequence as `xz -d` reads them; `LZMA_FINISH` is given once the
input ends and `LZMA_STREAM_END` is the only success. A stream whose check
type is none, or one this `liblzma` cannot compute, decodes with
`xz:check` recorded in the layer report's `unverified` list. `LZMA_DATA_ERROR`
is `FSIMG_ECORRUPT` naming the compressed offset, which covers a damaged
block, a failed check, a failed header or index CRC32 and any trailing
byte that is not stream padding; `LZMA_BUF_ERROR` after the input has
ended is `FSIMG_ECORRUPT`, a stream truncated or followed by fewer
trailing bytes than a stream header, which liblzma cannot tell apart;
`LZMA_OPTIONS_ERROR` is `FSIMG_EUNSUPPORTED` naming the compression, for a filter or option this
`liblzma` does not decode; `LZMA_FORMAT_ERROR` after the claim is
`FSIMG_ECORRUPT`.

### EROFS block decoding

The [EROFS driver](design-erofs-driver.md#decode-windows) decodes a
compressed physical cluster into its logical extent: one bounded input, the
cluster's stream after the driver has stripped its leading zero bytes, at
most 1 MiB, into one bounded output, the extent's decoded length, at most
12 MiB, both buffers the driver owns ([limits](design-erofs.md#limits)).
Two of the format's four [algorithms](design-erofs.md#algorithms) are
these libraries': MicroLZMA through liblzma and Zstandard through libzstd.
Under the architecture's
[optional-integration rule](architecture.md#dependencies-and-implementation-organization)
each is a private entry point in the library's one source file, so `xz.c`
and `zstd.c` stay the only files that include a library header, and the
driver reaches it through the registry row.

**Entry point.** `fsimg_compression_ops_t` gains one operation,
`block_decode`, NULL on every row but these two. It takes the context, the
input bytes and their length, the output buffer and the exact length to
produce, a partial flag, and the memory bound in bytes that the EROFS
[configuration record](design-erofs.md#configuration) declares: the
MicroLZMA dictionary size, or the Zstandard window, `2^(window log + 10)`.
It fills the output and returns `FSIMG_OK`, or a status from the table
below, and writes nothing outside the output buffer. The driver finds the
algorithm's row by its layer name, `xz` or `zstd`; a row without
`block_decode`, because the build lacks the library or, for MicroLZMA, its
liblzma predates 5.4, is `FSIMG_EUNSUPPORTED` naming the library
(`liblzma 5.4` or `libzstd`), raised when a cluster of that algorithm is
read, so the rest of the volume stays readable. The driver has checked the
bound against the record's range before the row sees it.

**Decoding.** Each call creates a fresh decoder bound to a fresh adapter,
decodes in one pass over the whole input and output, and frees the decoder
before returning, so no state outlives a cluster and its memory returns to
the budget when the call ends.

* MicroLZMA: `lzma_microlzma_decoder(&stream, input length, output
  length, !partial, dictionary size)`, then one `lzma_code(LZMA_FINISH)`
  over the whole input and output. The compressed length is exact, since
  the stream is right-aligned in its cluster, and the uncompressed length
  is exact unless the reference is partial, in which case the decoder
  stops at the output length without requiring the stream to end there.
  `LZMA_STREAM_END` with the output full is the one success in both cases:
  liblzma returns it when the output reaches the given length under an
  inexact size as it does at the stream's end under an exact one, and
  `LZMA_OK` when the input ends first (observed on liblzma 5.8.4,
  [E9](../docs/erofs-research.md#e9-other-codecs)); the reference decoder
  accepts only `LZMA_STREAM_END` for either case (source `decompress.c`).
  The dictionary size comes from the record, not the stream, which carries
  none; the driver passes the smaller of the record's size and the
  extent's length, at least 4096, since a fresh decoder cannot reach back
  further than it has produced. liblzma allocates that much, and a stream
  compressed with a larger dictionary fails as `LZMA_DATA_ERROR`.
* Zstandard: `ZSTD_getFrameHeader` over the cluster's first bytes, the
  same guard the [zstd profile](#zstd) applies to every frame start, so
  that libzstd's legacy decoders, which allocate outside the adapter, never
  see a cluster: a legacy frame, a skippable frame, or bytes the parse
  rejects are `FSIMG_ECORRUPT`, as a cluster holds exactly one current
  frame. Then a `ZSTD_DStream` from `ZSTD_createDStream_advanced` with the
  adapter, `ZSTD_d_windowLogMax` set to the record's window log plus 10
  before the first byte, then `ZSTD_decompressStream` over the whole input
  until it returns zero, the frame's end, or the output is full. Without
  `partial` the frame must end with the output exactly full: a frame that
  ends short, or still has bytes to flush when the output is full, is
  `FSIMG_ECORRUPT`, and bytes after the frame's end are not judged. With
  `partial` a full output is success wherever the frame stands, and a frame
  that ends first is `FSIMG_ECORRUPT`. A frame whose window exceeds the
  record's bound is refused by the library before it allocates, and a frame
  that names a dictionary fails; both are `FSIMG_ECORRUPT`, not
  `FSIMG_EUNSUPPORTED` as in the whole-file profile, because the record pins
  what a cluster may need, so a cluster that needs more is damaged rather
  than a feature this build lacks. The producer writes frames with the
  content size, no checksum and no dictionary identifier (E9).

**Memory.** Every allocation the library makes goes through the adapter
and is charged to the context before the library uses it, as for the
whole-file decoders, since the header parse keeps legacy frames from
libzstd, so the dictionary or window is charged before the
first byte is decoded, and a budget that cannot hold it fails as
`FSIMG_ELIMIT` with `FSIMG_RESOURCE_MEMORY` naming the library, with
nothing decoded and accounting back at its pre-call value. The record makes
the need known before the stream is seen: at most 8 MiB for a MicroLZMA
dictionary and 1 MiB for a Zstandard window, plus the library's fixed
state. The output buffer is the driver's decode window, charged by the
driver ([resource bounds](design-erofs-driver.md#resource-bounds)); the
entry point allocates no buffer of its own.

**Build.** `lzma_microlzma_decoder` exists in liblzma 5.4.0 and later.
`configure` runs a second link test after `FSIMG_CODEC_LIBRARY([xz], ...)`
over the same `LZMA_CFLAGS` and `LZMA_LIBS`, calling that function, and
defines `FSIMG_MICROLZMA` when it links; `xz.c` compiles its `block_decode`
under that macro, so an older liblzma still serves xz and leaves the
operation NULL. The existing libzstd probe already calls
`ZSTD_d_windowLogMax` and `ZSTD_decompressStream`, so zstd needs no second
test. `--without-xz` or `--without-zstd` removes the block decoder with the
compression, and the MSan build, configured without both, reports both
EROFS codecs as not covered.

**Capability output.** `fsimg --formats -v` names under the `erofs` row
the cluster algorithms this build decodes and the library each depends on:
LZ4, the project codec, with no library or version, and each other
algorithm with the version its library's `version` operation gives, zlib's
for DEFLATE, in identifier order (`lz4; lzma, liblzma 5.8.4; deflate, zlib
1.3.1; zstd, zstd 1.5.7`), omitting an algorithm whose row has no block decoder. The text is
`fsimg_format_info_t.codecs`, which a format descriptor's optional `codecs`
operation writes, and `--json` gives it as the format record's `codecs`,
null for a format without compression. A user reads
from one listing which EROFS volumes a build can extract.

**Sandbox.** Both calls are pure computation over caller buffers plus
`malloc`: the MicroLZMA decoder is a single-threaded raw decoder, and
`ZSTD_decompressStream` with no workers starts no thread, opens no file and
asks the kernel nothing, so the worker's profile is unchanged and the
required-confinement checks of the whole-file decoders cover them.

**Refusals and error results.** Error mapping stays a closed table per
library in the file that owns it:

| Situation | Result |
| --- | --- |
| A cluster of an algorithm whose row has no `block_decode` | `FSIMG_EUNSUPPORTED` naming the library, when that content is read |
| The budget cannot hold the dictionary or window (`LZMA_MEM_ERROR`, `ZSTD_error_memory_allocation`) | The adapter's first error: `FSIMG_ELIMIT`, `FSIMG_RESOURCE_MEMORY`, naming the library, nothing decoded |
| The stream ends before the output is full; the output fills before the frame ends outside a partial reference; a cluster whose first bytes are not a current Zstandard frame header (a legacy or skippable frame, or a parse error); damaged data (`LZMA_DATA_ERROR`, `LZMA_BUF_ERROR`, `LZMA_OK` after `LZMA_FINISH`, `LZMA_OPTIONS_ERROR` from `lzma_code`, which is the stream's first byte holding properties out of range; `ZSTD_error_corruption_detected`, `_frameParameter_windowTooLarge`, `_dictionary_*`, `_prefix_unknown`, any other decode-side code) | `FSIMG_ECORRUPT` naming the algorithm; the driver adds its EROFS prefix |
| An output length of zero, a bound outside the record's range, `LZMA_OPTIONS_ERROR` from `lzma_microlzma_decoder`, `LZMA_PROG_ERROR`, `ZSTD_error_parameter_*`, `ZSTD_error_stage_wrong` | `FSIMG_ESTATE` with the call and code: the driver validated every parameter first |

The [tests table](#tests-and-evidence) carries the block decoders' rows.

### Settings

The [settings catalogue](design-settings.md) gains three rows, present in
every build; the [layer settings table](design-layers.md#settings) lists
them beside `gzip:level`:

| Key | Type | Default | Validation |
| --- | --- | --- | --- |
| `zstd:level` | integer | 3 | 1 to 19, zstd's regular levels; the ultra levels 20 to 22, which `zstd(1)` itself unlocks only with `--ultra` because they need a 128 MiB window on every reader and up to a gibibyte on the writer, are not written; only when zstd is selected |
| `xz:level` | integer | 6 | 0 to 9, the `xz(1)` presets; only when xz is selected |
| `xz:check` | `crc32`, `crc64`, `sha256` | `crc64` | `crc32` for readers that decode no other check (boot loaders, kernels); `none` is never written: an unchecked stream is read and reported, not produced |

`fsimg_layer_options_t` gains `zstd_level` and `xz_level` (`uint32_t`)
and `xz_check` of the public enum `fsimg_xz_check_t` (`FSIMG_XZ_CRC64 = 0`,
`FSIMG_XZ_CRC32`, `FSIMG_XZ_SHA256`), whose zero value is the default as
`FSIMG_DMG_ZLIB` is. `zstd_level` follows the rule of `gzip_level`: zero
selects the default, and the key refuses zero. Preset 0 is a real xz
preset, so zero cannot select the default for `xz_level`; the catalogue
row's initial value is 6, which `fsimg_layer_options_init()` stores, as
`fsimg_write_options_init()` stores the `reserve` row's 3. The members take the start of the structure's
reserved area, which shrinks so the structure's size does not change. The
worker's job record carries the three fields as
it carries `gzip_level`, in a compression block after the `ifsboot`
settings that its layout check documents. A key of a compression that is not selected fails as every
unselected layer's key does; a key of a compression this build does not
implement is accepted by the catalogue and the creation then fails as
unsupported, naming the compression, before any byte is written. No
read-side key exists: the window limit is a profile constant and the
compression is detected or forced by name.

### Untrusted input and bounds

Both decoders parse attacker-controlled bytes inside the worker. The rules
of the [layer security section](design-layers.md#security) hold, and three
facts bound each decode independently of the input:

* **Memory.** Every library allocation goes through the adapter and is
  charged to the context before use (for zstd, given the per-frame header
  parse above, which keeps legacy frames from the library), so the window
  or dictionary a frame
  declares is bounded by the memory budget: a frame the budget cannot hold
  fails as `FSIMG_ELIMIT` with `FSIMG_RESOURCE_MEMORY`, naming the
  compression, and leaves accounting at its pre-open value. zstd's
  `ZSTD_d_windowLogMax` of 27 additionally refuses frames above 128 MiB as
  unsupported before any allocation, so the memory a zstd decode can ask
  for is a known constant whatever the budget. fsimg's own buffers are
  64 KiB in and 256 KiB out, as gzip's are.
* **Output.** Decoded bytes are reserved against the spool limit in steps
  of at most 256 KiB before they are written; neither a declared content
  size nor an index record sizes anything. A declared size is only a value
  the decoded length must equal at the end.
* **Work.** Every compressed byte read and every byte produced is charged
  to the session's work counter, so `-l work=` and the deadline bound a
  decode and a stream stops with `FSIMG_RESOURCE_WORK`.

Diagnostics use bounded fsimg-owned text with the library's numeric code
and the compressed offset; `ZSTD_getErrorName()` and the library strings
are never copied into an error. Error mapping is a closed table per
library, in the file that owns the library:

| libzstd result | fsimg status |
| --- | --- |
| `ZSTD_error_memory_allocation` | The adapter's first error, or its `FSIMG_ENOMEM` fallback |
| `ZSTD_error_frameParameter_windowTooLarge` | `FSIMG_EUNSUPPORTED`: window above 128 MiB |
| `ZSTD_error_dictionary_wrong`, `ZSTD_error_dictionary_corrupted` | `FSIMG_EUNSUPPORTED`: the frame needs a dictionary |
| `ZSTD_error_frameParameter_unsupported`, `ZSTD_error_version_unsupported` | `FSIMG_EUNSUPPORTED` naming the compression |
| `ZSTD_error_prefix_unknown` after a frame | `FSIMG_ECORRUPT`: trailing bytes at the offset |
| `ZSTD_error_corruption_detected`, `ZSTD_error_checksum_wrong`, `ZSTD_error_srcSize_wrong`, any other decode-side code | `FSIMG_ECORRUPT` at the compressed offset, with the code number |
| Input ends with the decoder expecting more | `FSIMG_ECORRUPT`: truncated |
| `ZSTD_error_dstSize_tooSmall`, `ZSTD_error_parameter_*`, `ZSTD_error_stage_wrong`, any encode-side error | `FSIMG_ESTATE` with the call and code: fsimg's parameters are validated before the library sees them and its buffers are its own |

| liblzma result | fsimg status |
| --- | --- |
| `LZMA_MEM_ERROR` | The adapter's first error, or its `FSIMG_ENOMEM` fallback |
| `LZMA_MEMLIMIT_ERROR` | `FSIMG_ESTATE`: no memory limit is set |
| `LZMA_FORMAT_ERROR` after the claim | `FSIMG_ECORRUPT`: stream header or flags invalid |
| `LZMA_OPTIONS_ERROR` while decoding | `FSIMG_EUNSUPPORTED` naming the compression: a filter or option this liblzma does not decode |
| `LZMA_DATA_ERROR` | `FSIMG_ECORRUPT` at the compressed offset |
| `LZMA_BUF_ERROR` with the input ended | `FSIMG_ECORRUPT`: truncated, or trailing bytes shorter than a stream header |
| `LZMA_NO_CHECK`, `LZMA_UNSUPPORTED_CHECK` | Not errors: `xz:check` recorded as unverified; decoding continues |
| `LZMA_GET_CHECK`, `LZMA_PROG_ERROR`, `LZMA_OPTIONS_ERROR` while encoding, any other code | `FSIMG_ESTATE` with the call and code |
| `LZMA_STREAM_END` with no input left | Success |

Decode-side codes the tables do not list are corruption, because the
input is untrusted and the library's new code cannot be a parameter error
of fsimg's; encode-side codes they do not list are internal errors, because
the input is fsimg's own transcode. A stale adapter error never masks a
non-memory return: each call's result is classified first and the adapter
consulted only for the memory code.

### Verification scope

The [verification scope table](design-layers.md#verification-scope) gains:

| Layer | Checks |
| --- | --- |
| zstd | Each frame's content checksum where present and content size where declared, at spool time; trailing bytes. A frame without a checksum makes `zstd:checksum` unverified and the layer scope incomplete |
| xz | Each block's check (CRC32, CRC64 or SHA-256), each block's sizes against the index, the stream header, footer and index CRC32s and the header-footer flags tie, at spool time; trailing bytes other than stream padding. A stream whose check is none or unsupported makes `xz:check` unverified and the layer scope incomplete |

The incomplete scope uses the mechanism `hbcifs` uses: the entry joins the
layer report's `unverified` list, `W` names it beside the compression,
the JSON verify record is `"complete":false`, and the exit status is that
of a passing verification. Nothing else changes: a file with no integrity
check still decodes, the filesystem is verified as usual, and the report
says the compression's bytes were not checked rather than implying they
were.

### Sandbox

The decoders and encoders run inside the confined worker after
acknowledgement, on bytes the coordinator pumps or passes unread, and add
no descriptor, name authority or syscall to the profiles. The claim rests
on the library facts in [the evidence](#problem-and-evidence) and is
verified, not assumed: the CLI tests decode a zstd and an xz image and
create one of each under `-S required` on Linux (seccomp and Landlock) and
FreeBSD (Capsicum), where a syscall outside the allowlist fails with
`EPERM` or `ECAPMODE` and the operation fails rather than passing. The
Linux run also covers an arm64 host when one is available, since liblzma's
CPU-feature query differs there. A library that fails this gate is not
enabled; the design does not widen the allowlist for it.

### Reproducibility

Under reproducible mode the compressed bytes are a function of the input,
the settings and the library version: both encoders run single-threaded
with every parameter fsimg sets fixed, neither format carries a timestamp
or host identity, and the zstd frame header's content size, when it has
one, is the disk length. Compressed bytes are reproducible only for one
library version, as for zlib, and for zstd one build of it: libzstd picks
the match finder of its middle levels by the vector instructions it was
compiled with, so one version built for different CPU families can
encode a small input differently. The capability output and the plan report
name the version (`zstd 1.5.7`, `liblzma 5.8.4`) so a claim can state it.
`i` names the compression it detected but no version: the running
library did not necessarily produce the bytes it reads. The
[reproducibility section](design-layers.md#reproducibility) extends its
zlib sentence to the two libraries. The tests compare output across hosts
only within one version and record the version beside the bytes.

### Licensing

`libzstd` is offered under BSD-3-Clause or GPL-2.0 at the licensee's
choice; fsimg takes it under BSD-3-Clause, which is compatible with
BSD-2-Clause distribution and asks for the copyright notice and
disclaimer in a binary distribution. Linking the system shared library
imposes nothing on fsimg's source; a static distribution carries the
library's notice beside the existing `third-party/` notices. `liblzma` is
0BSD, which requires nothing. Neither library enters the repository, as
zlib does not; configure finds the host's copy so its security fixes apply
to the code that parses untrusted images, the argument the architecture
makes for zlib and the reason the 2024 liblzma supply-chain incident
(CVE-2024-3094) is a host update, not an fsimg release.

### Concurrency and failure

Nothing here is concurrent: the worker is single-threaded, both libraries
are told so, and the adapter adds no locks, as the zlib adapter adds none.
Failure follows the gzip profile: a decode failure discards the partial
spool with the image and releases every charge; an encoder failure marks
the output incomplete and the coordinator publishes nothing; a stream
reports incomplete output after its first byte. The library context is
freed exactly once on every path, including a failed open, with the
adapter and context alive, and the first error survives cleanup. A
`ZSTD_freeDStream`/`lzma_end` after an allocation failure releases what
the library allocated through the adapter, and the failure-injection tests
prove accounting returns to its pre-open value.

### Refusals and error results

| Situation | Result |
| --- | --- |
| `-a` with `.zst`/`.xz`, or the write options naming `zstd`/`xz`, in a build without the library | `FSIMG_EUNSUPPORTED` "writing this compression is not supported", naming it; CLI exit 1 after the rest of the command line is checked, writing nothing |
| A zstd or xz file read in a build without the library | Not surveyed; the file is unrecognized (`FSIMG_EFORMAT`) or, forced by name, `FSIMG_EUNSUPPORTED` "reading this compression is not supported" |
| `-o zstd:level=` or `xz:` key with the compression not selected | Unknown or unselected setting, before any worker starts |
| `zstd:level` outside 1 to 19, `xz:level` above 9, `xz:check=none` | `FSIMG_EINVAL` naming the key |
| A window above 128 MiB, a frame needing a dictionary, an xz filter this liblzma lacks | `FSIMG_EUNSUPPORTED` naming the compression and the fact |
| Budget cannot hold the window, dictionary or encoder | `FSIMG_ELIMIT`, `FSIMG_RESOURCE_MEMORY`, naming the compression |
| Decoded bytes would cross the spool limit | `FSIMG_RESOURCE_SPOOL` at the step that crosses it |
| Damaged data, failed checksum, trailing bytes, truncation | `FSIMG_ECORRUPT` naming the compression and the compressed offset |
| `hbcifs` inside zstd or xz, or on a stream | `FSIMG_EUNSUPPORTED` naming both layers, at the container claim |
| Pledged size mismatch, impossible library result | `FSIMG_ESTATE` with the call and code |

### Tests and evidence

Every passing case is paired with a seeded failure that proves the code
under test ran. Oracle availability is reported, not assumed: a missing
`zstd` or `xz` tool skips its gate explicitly, and a build without the
library skips the profile's rows explicitly and runs the refusal rows.

| Area | Passing cases | Seeded failures |
| --- | --- | --- |
| Build | `--with-zstd=yes` and `--with-xz=yes` configure where the libraries exist; `--without-*` builds and `--formats` shows no support and no version; `auto` without the library reports the choice; `make distcheck` from a build without the libraries includes `zstd.c` and `xz.c`; `pkg-config --static --libs libfsimg` names the linked libraries | `--with-zstd=yes` with `ZSTD_LIBS=-lnonexistent` fails configure naming zstd; `check-dist.sh` on a tree with `zstd.c` removed from `EXTRA_DIST` fails |
| Registry and CLI | `.zst` and `.xz` strip to the stem; `-a -f x.img.zst` and `x.img.xz` create; `-a -f x.dmg.xz` requires `dmg` in the stack; `--formats` lists both with versions in a full build | `-a -f x.img.zst` in a build without libzstd exits 1 with the unsupported message and writes nothing; `-z -a -f x.img.zst` is a syntax error |
| zstd write | `zstd -t` accepts output at levels 1, 3 and 19; `zstd -d` yields the raw image; `zstd -l` shows the disk length as content size and a checksum for a bare disk, and a checksum with no content size around a container; the bound is at least `ZSTD_compressBound(n)` for sampled `n` up to 2^40; byte-identical output across hosts and budgets under one libzstd version; `cW` verifies a `.zst` through the spool | `zstd:level=0` and `20` refused naming the key; a flipped frame byte fails `zstd -t` and fsimg's own verification; a transcode fault on a bare disk that delivers one byte less than pledged fails at `ZSTD_e_end` as `FSIMG_ESTATE`; allocation failure injected at each adapter call fails the open or the write and leaves accounting at its baseline |
| zstd read | `zstd` output at levels 1, 3, 19 and `--ultra -22` spools and lists as the raw image, from a file and from a pipe; concatenated frames; a leading and a middle skippable frame; a `--no-check` file verifies with `zstd:checksum` unverified, `"complete":false` and a passing status; a file piped to `zstd` (no content size) verifies complete; `i` prints `Compression: zstd` | Trailing garbage (`FSIMG_ECORRUPT` at its offset); a flipped checksum; a content size one more and one less than the data; a frame truncated at each of its byte offsets up to the first block; a `--long=28` window (`FSIMG_EUNSUPPORTED`); a frame with a dictionary ID (`FSIMG_EUNSUPPORTED`); a bomb failing at the spool step that crosses the limit before the file grows past it; a 32 MiB budget failing a 64 MiB-window frame (`zstd --long=26`) as a memory limit naming zstd, with nothing decoded; forced `zstd` on a gzip file (`FSIMG_EFORMAT`); a zstd file whose content is itself zstd is unrecognized |
| xz write | `xz -t` accepts output at presets 0, 6 and 9 with each check; `xz -d` yields the raw image; `xz -l` shows one stream, one block and the chosen check; the bound is at least `lzma_stream_buffer_bound(n)` for sampled `n` up to 2^40; byte-identical output across hosts under one liblzma version; `cW` verifies a `.xz` | `xz:level=10` and `xz:check=none` refused naming the key; preset 9 under a 256 MiB budget fails at encoder open as `FSIMG_RESOURCE_MEMORY` naming xz before any byte is written; a flipped block byte fails `xz -t` and fsimg's verification; allocation failure injected at each adapter call |
| xz read | `xz` output at presets 0, 6 and 9 and with each check spools and lists; `-T0` multi-block streams; concatenated streams with and without stream padding; `--check=none` verifies with `xz:check` unverified and a passing status; `i` prints `Compression: xz` | Trailing garbage, a padding run of 5 zero bytes (`FSIMG_ECORRUPT` at the offset where the decoder stopped, which liblzma places after the bytes it read as the next stream's header); a flipped check; a flipped index CRC32; a header flags byte changed; a stream truncated at each offset through its first block header; a 4 GiB dictionary size (`FSIMG_RESOURCE_MEMORY` at the allocation, nothing decoded); a bomb at the spool step; a `.lzma` (legacy) file is unrecognized, not an xz claim |
| EROFS block decoding | Every MicroLZMA cluster of the `e9-lzma-*` fixtures and every Zstandard cluster of the `e9-zstd-*` fixtures decodes through the row operation to its extent and matches the manifest (`test/erofs-integration.sh`), big clusters (`-big64k`, `-words6m-big1m`) and the dictionary extremes (`e9-lzma-dict8m`, `e9-zstd-dict1m`) included; shared clusters from `test/erofs-gen.c` decode under `partial` to the shorter extent; each decode charges and releases the dictionary or window | A flipped stream byte (`FSIMG_ECORRUPT` naming the algorithm); an extent length one more and one less than the stream produces (both `FSIMG_ECORRUPT`); a Zstandard frame built with `ZSTD_c_windowLog` 21 against a record of window log 10, and one with a dictionary identifier (`FSIMG_ECORRUPT`, nothing allocated above the bound); a cluster opening with a legacy magic in the v0.5 to v0.7 range libzstd decodes (`27 B5 2F FD`, v0.7) and one holding a skippable frame (`FSIMG_ECORRUPT`, no decoder created); a MicroLZMA stream compressed with a 1 MiB dictionary read under a 64 KiB record (`FSIMG_ECORRUPT`); a memory budget below the dictionary (`FSIMG_RESOURCE_MEMORY` naming the library, nothing decoded); allocation failure injected at each adapter call, accounting at its baseline; a build without the library, and a test build with `FSIMG_MICROLZMA` undefined, list the fixtures and read their LZ4 and uncompressed files while the lzma or zstd files fail as `FSIMG_EUNSUPPORTED` naming the library; `fuzz/erofs.c` runs with the libraries the build has |
| Spool and streams | Stdin of a `.zst` and a `.xz` image through the coordinator's pipe; `cf -` writing a zstd and an xz stream the oracles decode | Short write, flush failure and cancellation at each piece: staged files removed, nothing published, stream reports incomplete output after the first byte |
| Sandbox | Decode and create under `-S required` on Linux x86-64 and FreeBSD, and on Linux arm64 where available | A test build that sets `ZSTD_c_nbWorkers` to 1 fails under `-S required`, proving the gate sees a thread attempt |
| Adapter | Charged deltas and release for each binding; zero-byte requests; the first error kept across several allocations in one library call; poison mode | A binding that installs only one callback fails the contract test |
| Fuzzing | `fuzz/compression.c`, the gzip harness generalized and built once per compression the build implements (`fuzz-gzip`, `fuzz-xz`, `fuzz-zstd`): the input is a whole file opened forced, as a file and as a stream, into a fresh spool under a 4 MiB limit and a 32 MiB memory budget, under sanitizers | Corpus seeded from every fixture above and from each oracle's output at each level and check |

CI builds with the libraries on macOS (Homebrew), Ubuntu (`libzstd-dev`,
`liblzma-dev`) and FreeBSD (liblzma from the base system, libzstd from the
`archivers/zstd` port), and the GCC 16 job compiles them with GCC. The
GCC 13 job configures `--without-zstd --without-xz`, so every commit
proves both the integration and the core's independence from it.

### Alternatives considered

* **Project-owned decoders.** The LZO1X and NRV2B codecs are project-owned
  because their libraries are GPL and absent from the base systems. zstd
  and xz are the opposite case: permissively licensed, system libraries on
  FreeBSD, standard everywhere else, and each several thousand lines of
  performance-critical code whose decoders are the most-fuzzed codec code
  in existence. A project-owned decoder would prevent only the build-time
  variability this design reports honestly, at the cost of maintaining a
  second implementation of two formats that fsimg does not need to control.
* **libarchive.** Would read and write both compressions behind one API,
  but brings its own format detection, allocator and I/O model, which do
  not fit the spool and the bounded probe contract, and a dependency far
  larger than the two libraries it would wrap. The architecture already
  places libarchive outside the core for archive import and export, not
  for compression.
* **Always-on dependencies.** Would remove the conditional and the
  `--formats` variability, but would make ordinary raw-image creation and
  extraction on a macOS host depend on Homebrew, which the dependency rule
  forbids; zlib earned its place by being present on every target.
* **A separate shared object or plugin.** Would keep the core binary free
  of the libraries' symbols, but needs a mutable registry or a runtime
  loader, and the worker forbids plugin loading. The conditional source
  file keeps the registry static and the public header constant, which is
  the property that matters.
* **zstd accounting through the stable API only.** Would avoid the
  `ZSTD_STATIC_LINKING_ONLY` section: set `ZSTD_d_windowLogMax`, decode the
  frame header's one-byte window descriptor (RFC 8878 section 3.1.1.1.2)
  to pre-charge the window plus a fixed context allowance through a new
  context memory-reservation primitive, and reconcile with
  `ZSTD_sizeof_DStream()` after each call. It removes one ABI dependency at
  the cost of a new primitive, a second header parser and after-the-fact
  reconciliation, where the custom allocator charges every allocation
  before use with the construction zlib and liblzma already share. The
  allocator symbols have not changed since zstd 1.0, the shared libraries
  fsimg builds against export them, and configure's link test refuses a
  library without them.
* **A read-side window-limit setting.** Would let a user raise the 128 MiB
  zstd limit for `--long=31` files. Such files are rare, the limit matches
  the `zstd` tool's own default, and the memory budget already decides
  what a decode may hold; a setting would add a read-side key to a layer
  that fills no slot. The refusal names the limit and the user decompresses
  with `zstd --long=31` first.

