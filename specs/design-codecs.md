# Compression codecs: LZO1X and NRV2B

## Scope

fsimg owns three stream codecs: an LZO1X decoder, an NRV2B decoder for the
8-bit control-buffer variant, and an LZO1X encoder. They live in
`lib/codec/`, are private to libfsimg, and know nothing about the images
they serve. A codec sees one compressed stream and one output; it never sees
a chunk length, a header or a checksum. The framing of QNX boot images, a
sequence of chunks each introduced by a 16-bit big-endian stored length, is
the `qnxboot` container's in the [layer design](design-layers.md); the
single unframed stream behind the 64-byte Harman-Becker header is the
`hbcifs` container's, in the same design. Both containers call the decoders
specified here, and the `qnxboot` wrap calls the encoder.

The codecs are project-owned rather than linked from `liblzo2` and `libucl`,
and the [IFS plan](../docs/ifs-plan.md#codecs-and-dependencies) records why:
both libraries are GPL-2.0-or-later, which a BSD-2-Clause library cannot
carry into every binary built on it; the working agreement allows no runtime
dependency beyond libc, the host and the system zlib, and neither library is
a system library on macOS or FreeBSD; an optional link would leave the
default build unable to read any PCM image, the main target; and the
decoders are small, a few hundred lines each, most of it the bounds checks
and tests fsimg needs whichever code decodes.

## The streams to be read and written

| Evidence | Codec | Shape |
| --- | --- | --- |
| H3 boot images, `PCM3_IFS1*` and `PCM3_Emergency*` (eight SH-4 images, source S1 in the [research record](../docs/ifs-research.md#s1-h3-boot-images)) | LZO1X | 120 to 294 framed chunks per image, each one complete stream; decoded chunks of 65536 bytes with 4096 and 61440 pairs in the `PCM3_IFS1` family; stored chunks up to 65059 bytes. In `PCM3_Emergency_LOW.ifs` every one of the 120 streams ends in the three-byte end marker `11 00 00` and the stored lengths run from 2326 to 48714 bytes |
| H3 `PCM3_IFS2.ifs` (`hbcifs`) | LZO1X | One unframed stream of 33575140 bytes decoding to 68849668 bytes, consumed exactly, ending in `11 00 00` |
| X1 `qnxbase*.ifs` (three x86 images, source S2) | NRV2B | 51 framed chunks per image, 50 of them decoding to 65536 bytes |
| `mkifs compress=2` and `compress=3` output from QNX 6.5.0 (E7) | both | The same framing on small bodies, for the gates below |

The `hbcifs` stream is why the LZO1X decoder is resumable: a 68 MiB output and a
33 MiB input cannot both sit in memory under the 16 MiB minimum budget, so
the container feeds the decoder input in pieces and drains the output to the
spool as it is produced. A framed chunk is the one-call case of the same
interface.

Two documents supply the stream formats. The LZO1X description is the Linux
kernel's `Documentation/staging/lzo.rst`, prose that documents what the
kernel's safe decompressor accepts, paraphrased here in this design's own
tables. The NRV2B description is a facts-only note written by a separate
describer under the [clean-room procedure](#clean-room-procedure). Every
fact that mattered was checked against the black-box oracle, a wrapper
around the public APIs of `liblzo2` 2.10 and `libucl` 1.03
(`tools/qnx/oracle/oracle.c`, built into
`~/.local/share/fsimg-oracles/ifs-codecs/`), by decoding hand-built streams.
Where the kernel document and the oracle disagree, the oracle wins and the
disagreement is noted.

## Central construction

The LZO1X decoder is a small explicit state machine that moves bytes
between two windows the caller owns and stops at whichever edge it reaches
first. It advances one input byte at a time. Its state is a phase and a few
counters: which part of an instruction is being read, how many bytes of the
current literal run or match remain, the match distance and the literal
state. Every transition either consumes a byte or produces one, so a call's
running time is proportional to the bytes it moves, and every transition
checks the one bound it touches, so no operand is trusted before it is
checked.

The NRV2B decoder is one function over one whole framed chunk. The only
NRV2B streams fsimg reads are chunks whose stored length is a 16-bit word
and whose decoded size is at most 65536 bytes, so input and output both fit
a buffer and nothing is gained by resuming. It reads control bits and data
bytes with the same discipline, one at a time with the bound checked before
each, and copies matches the same way.

Everything the plan asks for follows from this shape:

* **No read past the input and no write past the output.** A decoder takes
  one byte when one is available and writes one byte when there is room;
  there is no look-ahead and no block copy. A match is a counter and a
  distance, copied one byte at a time from the output, which is also what
  makes overlapping matches (distance shorter than length) produce the
  repeating runs both formats rely on.
* **LZO1X is resumable at any byte.** Because no instruction is parsed as
  a unit, input may end in the middle of an instruction and output may
  fill in the middle of a copy; the next call continues from the same
  state. A framed chunk is decoded in one call; the `hbcifs` stream in as
  many as the caller likes. Decoding is invariant under where the caller
  splits its windows, which the tests exploit.
* **The output bound is the caller's.** For LZO1X two numbers bound the
  output, both from the caller: the total the whole stream may produce,
  given when the decoder is initialized, and the room in the current
  window. For NRV2B the one call's capacity is the bound. A length read
  from the stream is never trusted; it is a counter held against the
  remaining bound as it is accumulated, before any copying begins, with
  the one exception the LZO1X end marker forces, stated under
  [check order](#decoder-contract).
* **Work is bytes moved.** The caller charges consumed plus produced bytes
  to the session's work counter after each call, the same unit the layer
  design charges for zlib chunks, so `-l work=` and the deadline bound
  decompression. A decompression bomb is bounded twice: a claimed length
  beyond the bound is corrupt at the header, and a claim within it is paid
  for byte by byte at the window's pace.
* **No allocation.** The LZO1X decoder state is a struct the caller embeds
  or keeps on its stack; the NRV2B decoder has no state outside its call;
  the buffers are the caller's. The encoder's one table is allocated once
  through the resource context by its owner and reused per block.

## The LZO1X stream

An LZO1X stream is a sequence of instructions. Each instruction is one byte
whose top bits select the class, followed by the operands the class needs,
followed by up to three literal bytes the instruction carries along. A
literal run instruction copies a counted run of bytes from the input; a
match copies bytes already produced, from a distance behind the end of the
output. The stream ends with an end marker, which is a far match of a
distance no real match can have.

### Literal state

The meaning of an instruction byte below 16 depends on how many literals the
previous instruction copied. The decoder carries this as the **literal
state**: 0 after a match that carried no literals, 1 to 3 after a match
that carried that many, and 4 after a literal run. The state exists only
to disambiguate the `0000 xxxx` byte; no other class reads it.

### First byte

| First byte | Meaning |
| --- | --- |
| 0 to 17 | An ordinary instruction, decoded with literal state 0. Bytes 0 to 15 are therefore a long literal run; 16 and 17 are far matches, which with nothing produced yet can only be the end marker (`11 00 00` is the complete stream for empty input) or corrupt. |
| 18 to 255 | A short first run of `byte - 17` literals, 1 to 238. The literal state becomes the count for 1 to 3 literals and 4 for more. |

The kernel document gives these ranges as 18 to 21 for "0 to 3" literals
and 22 to 255 for "4 to 238", which contradicts its own `byte - 17` formula.
The oracle's encoder writes `12` for one literal and `15` for four, and its
decoder treats the instruction after `15 41 42 43 44` with literal state 4;
the formula is right and the range boundaries in the document are off by
one. The document's version byte (a first byte of 17 introducing the
kernel's run-length extension) is a kernel-only extension; `liblzo2`
decodes 17 as an ordinary instruction, and so does fsimg. No producer of
IFS bodies emits the extension, and a stream that used it is rejected as
corrupt (trailing bytes, or a distance before the start) rather than
decoded wrongly.

### Instruction classes

`L`, `D`, `H` and `S` bits are read from the instruction byte and its
operands as shown; `n` is the decoded run or match length, `d` the
distance behind the current end of the output.

| Instruction byte | Literal state | Class | `n` | `d` | Operands after the byte |
| --- | --- | --- | --- | --- | --- |
| `0000 LLLL` (0 to 15) | 0 | long literal run | `3 + L`; `L = 0`: `18 + ext` | - | extension bytes; literal state becomes 4 |
| `0000 DDSS` (0 to 15) | 1 to 3 | short match | 2 | `1 + D + 4H` | one byte `H`; literal state becomes `S` |
| `0000 DDSS` (0 to 15) | 4 | short match, mid distance | 3 | `2049 + D + 4H` | one byte `H`; literal state becomes `S` |
| `0001 HLLL` (16 to 31) | any | far match | `2 + L`; `L = 0`: `9 + ext` | `16384 + 16384H + D` | extension bytes, then a 16-bit little-endian word `4D + S` (`D` is 14 bits); `d = 16384` is the end marker; literal state becomes `S` |
| `001L LLLL` (32 to 63) | any | long match | `2 + L`; `L = 0`: `33 + ext` | `1 + D` | extension bytes, then a 16-bit little-endian word `4D + S`; literal state becomes `S` |
| `01LD DDSS` (64 to 127) | any | near match | `3 + L` | `1 + D + 8H` | one byte `H`; literal state becomes `S` |
| `1LLD DDSS` (128 to 255) | any | near match | `5 + L` | `1 + D + 8H` | one byte `H`; literal state becomes `S` |

Facts the table relies on:

* **Length extension.** When the length field `L` is zero, the length
  continues in the following bytes: a run of zero bytes, then one nonzero
  byte. The extension value is `255 x (number of zero bytes) + nonzero
  byte`, so each zero byte adds 255 and the nonzero byte adds 1 to 255. The
  extension comes before the distance operand. `20 f3 10 00` from the
  oracle's encoder is a long match of `33 + 243 = 276` bytes at distance 5.
* **Distance ranges.** Short matches reach 1 to 1024; mid-distance short
  matches 2049 to 3072; near matches 1 to 2048; long matches 1 to 16384;
  far matches 16385 to 49151. 49151 is the largest distance any LZO1X
  stream can express.
* **Carried literals.** After its operands, a match carries `S` (0 to 3)
  literal bytes, copied before the next instruction is read. `S` is always
  the low two bits of the second-to-last byte of the instruction group: the
  instruction byte itself for near and short matches, the low byte of the
  16-bit word for long and far matches. A literal run carries none; after a
  run the next instruction is read with literal state 4, so two literal
  runs can never be adjacent, and a run of four or more literals after a
  match is written with `S = 0` followed by a long literal run.
* **End marker.** A far match whose distance is exactly 16384 (`H = 0`,
  `D = 0`) ends the stream. Its `L` field and `S` bits are decoded and
  ignored: the oracle accepts `11 01 00` (`S = 1`) and `10 01 00 00`
  (`L = 0` with an extension) as end markers after a literal. The encoder
  writes the canonical `11 00 00`. The decoder needs no special case: it
  reads the far match's length and word as for any far match and tests the
  distance.
* **Order.** Instruction byte, length extension if any, distance operand,
  carried literals.

### Worked streams

Each stream was decoded by the oracle to the output shown.

| Stream | Output | Reading |
| --- | --- | --- |
| `11 00 00` | empty | End marker alone |
| `12 41 11 00 00` | `A` | First run of one literal, end marker |
| `12 41 00 00 11 00 00` | `AAA` | After one literal (state 1) `00` is a short match: `D = 0`, `H = 0`, distance 1, length 2 |
| `13 41 42 04 00 11 00 00` | `ABAB` | Two literals (state 2); `04` is a short match with `D = 1`, distance 2 |
| `15 41 42 43 44 4d 00 45 00 00 11 00 00` | `ABCDABCEEE` | Four literals (state 4); `4d 00` is a near match of 3 at distance 4 carrying one literal `E`; state 1 makes `00 00` a short match at distance 1 |
| `15 41 42 43 44 21 0d 00 45 11 00 00` | `ABCDABCE` | `21` is a long match of 3; the word `000d` gives `D = 3`, distance 4, `S = 1` |
| `15 41 42 43 44 80 00 11 00 00` | `ABCDDDDDD` | `80` is a near match of 5 at distance 1, overlapping itself |
| `00 03` + 21 bytes + `11 00 00` | the 21 bytes | First byte 0 with state 0: long run, `L = 0`, extension 3, so `18 + 3` literals |
| `02` + 5 zero bytes + `20 f3 10 00` + `00 01` + 19 zero bytes + `11 00 00` | 300 zero bytes | The oracle's encoding of 300 zeros: a run of 5, a long match of 276 at distance 5, a run of 19 |

Streams the oracle rejects: `15 41 42 43 44 00 00 11 00 00` (a mid-distance
short match at 2049 with four bytes produced), `13 41 42 08 00 11 00 00`
(distance 3 after two bytes), `12 41 19 00 00` (a far match with `H = 1`,
distance 32768, not an end marker), `12 41 11 00 00 00` (a byte after the
end marker), and every proper prefix of a valid stream.

## The NRV2B stream

An NRV2B stream mixes **control bits**, packed eight to a byte, with
**data bytes**: literals and the low bytes of match offsets. There is no
header, length, checksum or padding between items, and the decoded size is
stored nowhere; the stream ends with an end marker. Matches copy bytes
already produced in the same stream; there is no preset dictionary.

### Control bits

Control bits are taken most significant bit first from the current control
byte. A new control byte is read from the input only when a control bit is
needed and the current byte has none left; at the start there is no current
byte. A data byte is always the next unread input byte at the moment it is
needed, even when the current control byte still has bits left. The two
interleave as the decoder consumes them: the control byte holding the next
bits was read earlier than the data bytes that follow it. The unused
low-order bits of the stream's last control byte carry nothing; the end
marker finishes before them, and the oracle decodes example A below with
all five unused bits set exactly as with them clear.

| Control bit | Item |
| --- | --- |
| 1 | Literal: the next data byte is appended to the output. |
| 0 | Match or end marker: an offset part, then, unless it is the end marker, a length part. |

### The variable-length number

The offset's high part and the long form of the length use one code for an
unsigned integer `v >= 2`. The value starts at 1 and is extended by pairs of
control bits: the first bit of a pair is appended to the value as its new
low bit, the second bit is a stop flag, 1 for the last pair. A number with
`n` significant bits takes `n - 1` pairs.

| `v` | Pairs (data, stop) | Bits |
| --- | --- | --- |
| 2 | (0,1) | `01` |
| 3 | (1,1) | `11` |
| 4 | (0,0) (0,1) | `0001` |
| 5 | (0,0) (1,1) | `0011` |
| 7 | (1,0) (1,1) | `1011` |
| 0x1000002 | 22 x (0,0), (1,0), (0,1) | 48 bits |

### Match offset

After the 0 control bit, the decoder reads one such number, `H`.

| `H` | Meaning |
| --- | --- |
| 2 | Reuse the last offset. No data byte follows; the last offset is unchanged. |
| 3 or more | New offset. The next data byte is `L`. The raw value is `(H - 3) x 256 + L`. A raw value of 0xFFFFFFFF is the end marker. Otherwise the offset is `raw + 1`, and it becomes the last offset. |

* The last offset starts at 1 in every stream, so a reuse may come before
  any new offset.
* An offset `d` copies from `d` bytes before the current end of the output;
  1 repeats the previous byte. Offsets 1 to 256 have `H = 3`, 257 to 512
  have `H = 4`, and so on.
* `L` is read as soon as `H` is complete, before any further control bit,
  even when `H` ended on the last bit of a control byte.
* `H` may not exceed 0x1000002; a stream whose `H` grows past that while
  being read is corrupt. With this limit the raw value 0xFFFFFFFF arises only
  from `H = 0x1000002` with `L = 0xFF`, so the end-marker test never relies
  on wrap-around. `H = 0x1000002` with `L = 0xFE` is a match with offset
  0xFFFFFFFF, which no output is long enough to satisfy; it is rejected as
  a distance before the output start.
* A reused offset is subject to the length bonus below exactly like a newly
  coded one.

### Match length

After the offset part, two control bits `a` and `b` give the length field
`F`:

| `a b` | `F` |
| --- | --- |
| `0 1` | 1 |
| `1 0` | 2 |
| `1 1` | 3 |
| `0 0` | A variable-length number `G` follows; `F = G + 2`, so 4 or more |

The bytes copied are `C = F + 1`, plus one more when the offset is greater
than 0xD00 (3328).

| Offset | `C` for `F = 1, 2, 3, 4, ...` |
| --- | --- |
| up to 0xD00 | 2, 3, 4, 5, ... |
| above 0xD00 | 3, 4, 5, 6, ... |

The reference encoder never writes a two-byte match above 0xD00 and splits
long matches into pieces that reuse the offset; the decoder accepts any
length the code expresses, subject to the caller's bounds.

### Copying, end marker and final state

* A match copies one byte at a time, front to back, each appended before
  the next is read, so an offset shorter than the count repeats a run with
  period equal to the offset: offset 1 with `C = 5` after `x` gives
  `xxxxxx`; offset 2 with `C = 6` after `ab` gives `abababab`.
* A match is valid only when its offset is at most the number of bytes
  already produced in this stream; an offset equal to that number starts at
  the first output byte. A reuse at the very start (last offset 1, nothing
  produced) is corrupt.
* The end marker is a 0 control bit, the number 0x1000002 (48 control
  bits), and the data byte 0xFF. It ends every stream, including the empty
  one, whose complete encoding is `00 00 00 00 00 04 80 FF`. Because the
  data byte follows the control byte that holds the marker's last bit, an
  encoded stream always ends in 0xFF.
* The input must end exactly with the marker's 0xFF; a byte after it is
  corrupt, and a stream that ends before it is corrupt.

### Worked streams

Each stream decodes under the oracle to the output shown. Bits are listed
in the order they are used.

| Example | Stream | Output | Reading |
| --- | --- | --- | --- |
| A | `C0 41 42 00 00 00 00 01 20 FF` | `AB` | `C0`: 1, literal `41`; 1, literal `42`; 0, match; `00000` begin `H`. Four zero bytes continue it. `01`: seven more zero bits, then data 1. `20`: stop 0, pair (0,1); `H = 0x1000002`. `FF`: end marker. Five unused bits remain in `20`. |
| B | `D9 61 62 01 80 00 00 00 00 02 40 FF` | `abababab` | `D9` = `1101 1001`: literals `a`, `b`; 0 match; `11` gives `H = 3`, so `L = 01` is read at once: offset 2; `00` long length; data 1. `80`: stop 1, `G = 3`, `F = 5`, `C = 6`, copied from 2 back; then the end marker. |
| C | `BA 61 00 62 50 00 00 00 00 00 48 FF` | `aaaabbb` | Literal `a`; match `H = 3`, `L = 00`, offset 1, `F = 2`, `C = 3`; literal `b`; match `H = 2` reuses offset 1 with `F = 1`, `C = 2`; end marker. |
| D | `94 78 00 00 00 00 00 12 FF` | `xxx` | Literal `x`; match `H = 2` reuses the initial last offset 1, `F = 1`, `C = 2`; end marker. |
| E | `00 00 00 00 00 04 80 FF` | empty | The first bit selects a match; 48 bits of `H = 0x1000002`; `FF`. |
| F | 0xD01 literals, a match with `H = 16`, `L = 00` (offset 0xD01) and `F = 1`, a reuse with `F = 1`, the marker | 0xD01 + 3 + 3 bytes | Both matches copy three bytes because the offset exceeds 0xD00; with offset 0xD00 the first copies two. |

## Decoder contract

The decoders share the step record and the error contract, declared in
`lib/codec/codec.h`; their shapes differ because their streams do:

```c
typedef struct fsimg_codec_step {
	size_t	 consumed;	/* input bytes used by this call */
	size_t	 produced;	/* output bytes written by this call */
	bool	 done;		/* the end marker has been decoded */
} fsimg_codec_step_t;

void fsimg_lzo1x_decoder_init(fsimg_lzo1x_decoder_t *, uint64_t limit);
fsimg_status_t fsimg_lzo1x_decode(fsimg_lzo1x_decoder_t *,
    const uint8_t *in, size_t in_len, bool final,
    uint8_t *window, size_t history, size_t capacity,
    fsimg_codec_step_t *, fsimg_error_t *);
fsimg_status_t fsimg_nrv2b_decode(const uint8_t *in, size_t in_len,
    uint8_t *out, size_t capacity, fsimg_codec_step_t *, fsimg_error_t *);
```

* **LZO1X limit.** `limit` is the most bytes the whole stream may produce.
  It is the caller's: the chunk size for a framed chunk, the header's
  decoded length for an `hbcifs` stream (itself checked against the spool
  budget by the container before the decoder sees it). A literal run or
  match whose length would take the stream past the limit is corrupt the
  moment the length is known, before any byte of it is copied. For every
  class but the far match, the accumulating length is compared with the
  remaining limit after every extension byte. A far match is held against
  the limit only once its distance word shows that it is not the end
  marker, because the marker is itself a far match of length 3 (`11 00
  00`) or longer (`10 01 00 00`) and must be accepted when the output
  already equals the limit, as it does at the end of every full chunk and
  of the `hbcifs` stream. Until the word is read, the far match's length
  is held to a fixed ceiling of 2^32 - 1, beyond which the stream is
  corrupt, so the accumulator cannot overflow and a marker never carries
  an unbounded prefix.
* **NRV2B bounds.** One call decodes one whole stream. `in_len` is at most
  65535 and `capacity` at most 65536, the framed chunk's limits; a larger
  value is `FSIMG_EINVAL`. The capacity is the limit: a literal when the
  output is full, or a match whose count would not fit, is corrupt before
  any byte of it is copied, and the accumulating `G` is held against the
  remaining capacity as it grows. A successful call has `done` set and
  `consumed == in_len`; the output is `out[0..produced)`.
* **LZO1X input.** `in[0..in_len)` is the next piece of the stream.
  `final` says no byte follows it. The call consumes bytes from the front
  and reports how many.
* **LZO1X window.** `window[0..history)` holds the most recently produced
  bytes, in order, and the call writes into
  `window[history..history + capacity)`. The caller keeps enough history
  for every valid match: at least `min(total produced, 49151)` bytes,
  since no LZO1X distance exceeds 49151. A one-call decode has
  `history = 0` and a window the size of the limit. The `hbcifs` container
  drains its window to the spool when it fills, moves the last 64 KiB to
  the front and continues.
* **LZO1X stopping.** A call returns `FSIMG_OK` only at an edge: its next
  transition needs an input byte beyond its input or an output byte beyond
  its capacity, or it has decoded the end marker; `done` reports the last.
  Instruction bytes that produce nothing are still consumed once the
  window is full, so a stream whose output fills the window exactly ends
  in that call. A caller's loop therefore
  ends every call by presenting the next input piece or draining the
  window, and the loop finishes within the stream's length plus the limit.
* **Matches.** The copy source is `distance` bytes before the write
  position, one byte at a time front to back. A distance greater than the
  bytes the stream has produced reaches before its first byte and is
  corrupt; an NRV2B offset of 0xFFFFFFFF always is. For LZO1X, let `total`
  be the bytes produced before the match and `available` the history plus
  the bytes produced by this call so far: a distance within `total` but
  greater than `available` means the caller kept too little history; that
  is a contract failure, `FSIMG_ESTATE`, never blamed on the image. The
  image's own verdicts on a match, its distance against `total` and a far
  match's length against the limit, come before it. A match still being
  copied when a call returns is checked again when the next call starts,
  against that call's history, so a caller that keeps less between calls
  gets the same `FSIMG_ESTATE` rather than a copy from outside its window.
* **Verdicts the decoder owns.** The three corrupt cases of the plan and
  the two framing cases all end in the decoder, because the caller tells it
  where the stream ends:

| Condition | Result |
| --- | --- |
| Input exhausted before the end marker: LZO1X with `final` set, NRV2B always | `FSIMG_ECORRUPT`, unterminated stream |
| LZO1X input exhausted before the end marker, not `final` | `FSIMG_OK`, more input needed |
| Bytes remain after the end marker, in this call or a later one | `FSIMG_ECORRUPT`, trailing bytes |
| Literal run or match would exceed the limit or capacity | `FSIMG_ECORRUPT`, output longer than its limit |
| Match distance greater than the bytes produced, including an NRV2B offset of 0xFFFFFFFF | `FSIMG_ECORRUPT`, distance before the output start |
| NRV2B `H` above 0x1000002 | `FSIMG_ECORRUPT`, offset out of range |
| LZO1X far-match length above 2^32 - 1 before its distance word | `FSIMG_ECORRUPT`, length out of range |
| LZO1X distance within the stream but beyond the kept history | `FSIMG_ESTATE`, history contract |
| LZO1X call after an error, or after `done` with more input | `FSIMG_ESTATE` and `FSIMG_ECORRUPT` respectively |
| `NULL` buffer with a nonzero length; `history + capacity` overflowing; NRV2B `in_len` above 65535 or `capacity` above 65536 | `FSIMG_EINVAL` |

  Trailing bytes are corrupt rather than reported for the container to
  judge because every framing fsimg reads gives the exact stream length:
  the chunk's stored length word and the `hbcifs` payload length, which the
  corpus shows consumed exactly. Both reference libraries reject trailing
  input too, so the differential checker compares like with like, and no
  caller has to remember a comparison. A container that needs a stream
  found by its end marker inside padding does not exist; such a container
  is the occasion to add a flag, not a reason to weaken the rule for the
  two that exist.

* **Errors.** A corrupt verdict sets the error with the codec's name, the
  check that failed and the stream offset (bytes consumed so far), through
  `fsimg_error_set`. The container adds the chunk or image it was decoding.
  After an error an LZO1X decoder object is finished; in either codec the
  bytes already in the output are the correct prefix of a corrupt stream
  and are never published.
* **Work.** The caller charges `consumed + produced` to its session after
  each call with `fsimg_session_charge`. The decoder's cost per call is
  proportional to that sum: every LZO1X transition consumes a byte or
  produces one; every NRV2B item either consumes a data byte, or is a match
  of five or more control bits producing at least two bytes, or is the
  end marker's 49 control bits and one data byte, at most eight bytes.

* **Check order.** Each field is checked the moment it is complete, in
  stream order, with one stated exception. For LZO1X the length and its
  extension come before the distance operand, so a long literal run, a
  near, short or long match is first held against the limit as its length
  accumulates and then, for a match, against the bytes produced when its
  distance is known. The exception is the far-match class: its length is
  held only to the 2^32 - 1 ceiling while it accumulates, then its
  distance word is read; a distance of 16384 ends the stream and the
  length is discarded; any other distance is checked against the bytes
  produced and only then is the length held against the limit. For NRV2B
  the offset comes first: `H` is held to 0x1000002 as it accumulates, the
  offset is checked against the bytes produced once `L` is read, and the
  length is held against the remaining capacity as `G` accumulates.
  Carried literals after an LZO1X match and the literals of a first run
  are held against the limit as a count, like a long run's length: a
  first run when its first byte is read, carried literals once their
  match has been copied, in both cases before the first of them is
  written. Every LZO1X copy therefore starts within the limit, and no
  byte of it is checked against the limit again. A stream that breaks
  two rules therefore has one
  definite verdict, the first in this order, and the unit tests expect it
  exactly.

The reference libraries report their own codes (`liblzo2` returns -4 for
truncated input, -5 for a full output, -6 for a distance before the start
and -8 for trailing bytes; `libucl` returns -201, -202, -203 and -205 for
the same four), but their check order differs from fsimg's and from each
other's where a stream breaks two rules, so the differential checker never
compares codes. It compares accept against reject, and on accept the output
bytes. Two behaviours of `libucl` 1.03 that the describer observed are not
copied: its safe decoder accepts a match whose
last byte lands one past the capacity and writes that byte (the
eight-byte `abababab` stream decodes "successfully" at capacity 7), and it
fetches a control byte without checking the input bound, so a truncated
stream can be read about one byte past its end before -201 is reported.
fsimg checks every byte it reads and never writes past its output; the
accepted language is otherwise the reference's.

## LZO1X encoder

```c
fsimg_status_t fsimg_lzo1x_encoder_new(fsimg_ctx_t *,
    fsimg_lzo1x_encoder_t **, fsimg_error_t *);
void fsimg_lzo1x_encoder_free(fsimg_lzo1x_encoder_t *);
size_t fsimg_lzo1x_encode_bound(size_t in_len);
fsimg_status_t fsimg_lzo1x_encode(fsimg_lzo1x_encoder_t *,
    const uint8_t *in, size_t in_len, uint8_t *out, size_t capacity,
    size_t *out_len, fsimg_error_t *);
```

The encoder turns one block into one complete stream, end marker included,
in one call. It need not reproduce `mkifs`'s bytes; it must be
deterministic, bounded and decodable by every reader that matters.

### Determinism

The stream is a function of the input bytes alone. The encoder's only state
is a position table of 16384 entries of 32-bit input offsets (64 KiB),
allocated and zero-filled once by `fsimg_lzo1x_encoder_new` through the
resource context. Candidates are offsets, never pointers; the hash is a
fixed function; there is no randomness, no reading of uninitialized
memory, no dependence on the output capacity beyond the bound, and no
arithmetic whose result depends on the host's word size. The table's
contents when a call begins do not matter either, so it is not reset
between blocks. This rests on one invariant of [matching](#matching):
before position `p` is examined, every position `q < p` of the current
block with a complete four-byte group has been recorded in its bucket,
including the positions inside matches. A change that skips recording any
position breaks it and must reset the table per block. Given the invariant: a table entry is only ever a candidate, accepted after its
position is checked to lie before the current one within 49151 bytes and
its four bytes are compared with the input. An entry left by an earlier
block is therefore rejected as a future position, rejected because its
bytes differ, or overwritten by the current block's own scan before it can
be read, since equal bytes hash to the same entry. The same block gives the
same stream on every host, in every process, after any sequence of earlier
blocks, and under a sanitizer with poisoned allocations.

### Matching

Scanning position `p` from 0:

* The hash of the four bytes at `p`, read as a little-endian 32-bit value,
  multiplied by 2654435761 modulo 2^32, top 14 bits, selects one table
  entry. The entry is the candidate; `p` replaces it.
* The candidate is a match when it lies before `p`, within 49151 bytes, and
  its four bytes equal those at `p` (the bytes are compared, so a hash
  collision is harmless). The match is extended forward while bytes are
  equal and input remains; this is greedy, with no lazy evaluation.
* A match is emitted when it is at least 4 bytes long at a distance of at
  most 2048, or at least 5 bytes long otherwise. Every position inside the
  match whose four-byte group lies within the input is recorded in the
  table, so later candidates do not depend on where earlier matches fell;
  scanning resumes after the match. Otherwise `p` advances by one and the
  byte becomes a pending literal. The last positions of the input, which
  have no complete four-byte group, and an input shorter than four bytes
  are emitted as literals without hashing.

### Instruction selection

| Distance | Length | Class written |
| --- | --- | --- |
| 1 to 2048 | 4 | near match `01LD DDSS` with `L = 1` |
| 1 to 2048 | 5 to 8 | near match `1LLD DDSS` with `L = n - 5` |
| 1 to 16384 | 5 or more (9 or more within 2048) | long match; `L = n - 2` up to 33, else `L = 0` and extension `n - 33` |
| 16385 to 49151 | 5 or more | far match; `L = n - 2` up to 9, else `L = 0` and extension `n - 9` |

An extension value `v` is written as `(v - 1) / 255` zero bytes followed by
the byte `v - 255 x (that count)`, which is 1 to 255. The encoder never
writes a short match: the two forms cost two bytes to cover two or three,
which the bound below has no use for.

Literals go in the first-byte form or ride on matches. The first run of
`n` literals is written as the byte `17 + n` for `n <= 238`, otherwise as
the byte `00` and the extension `n - 18`. After a match, 1 to 3 literals go
in that match's `S` field; 4 or more are written with `S = 0` as a long
run, `L = n - 3` up to 18, else `L = 0` and extension `n - 18`. Because `S`
counts the literals that follow the match, the encoder holds one pending
match and writes it when the next match or the end of input tells it the
count. The stream ends with `11 00 00`.

### Worst-case bound

`fsimg_lzo1x_encode_bound(n) = n + n / 255 + 6`. The encoder refuses a
capacity below the bound with `FSIMG_EINVAL` before writing anything, and
still checks every write against the capacity, reporting `FSIMG_ESTATE` if
the argument below ever fails; that status can only mean a defect in the
encoder, never an input the caller supplied.

The argument: a literal run of `n >= 4` costs its `n` bytes plus a header
of one byte, plus for `n >= 19` the extension's `1 + (n - 19) / 255` bytes,
so at most `2 + n / 255` beyond the literals; a first run of up to 238
costs one byte. Every match of `m` bytes costs at most `m - 2`: a near
match costs 2 for `m = 4` to 8; a long match costs 3 for `m = 5` to 33 and
`4 + (m - 34) / 255` beyond; a far match costs 3 for `m = 5` to 9 and
`4 + (m - 10) / 255` beyond. The stream has at most one more run with a
header than it has matches, so with `M` matches the headers cost at most
`2(M + 1) + n / 255` and the matches save at least `2M`, which with the
three-byte end marker gives `n + n / 255 + 5` in real arithmetic and
`n + n / 255 + 6` in integers. The encoder's input is at most 2^32 - 1
bytes, the table's offset width; a longer block is `FSIMG_EINVAL`.

A 65536-byte block has a bound of 65799, more than a 16-bit stored length
holds. The [`qnxboot` profile](design-layers.md#qnx-boot-image-qnxboot) in
the layer design owns the rule: the body is chunks of 65536 decoded bytes,
and a chunk whose encoding is longer than 65535 bytes is emitted as two
chunks of 32768, which always fit the bound.

### Validation

Three checks establish the encoder, in the order the plan runs them: every
stream it writes decodes to its input under the project's decoder; every
stream decodes to its input under the `liblzo2` oracle; and the native
startup code boots a wrapped image at the `qnxboot` wrap's native gate.

## Concurrency and failure

A codec object belongs to one caller and holds no reference to any other
object; there are no static tables and no global state, so independent
contexts may decode concurrently, and the single-threaded worker needs no
locking. A failed decode leaves an LZO1X decoder object finished and, in
either codec, the output holding `produced` correct bytes of a stream the
caller discards. A failed encode leaves the output buffer's contents
unspecified; the table needs no repair, since its contents never affect a
later block; nothing is allocated or freed by the call itself.
Cancellation and deadlines act between calls, where the container polls,
and a call is bounded by the window sizes the container chose.

## Module placement

| File | Owns |
| --- | --- |
| `lib/codec/codec.h` | `fsimg_codec_step_t`, the decoder and encoder types and prototypes; private to libfsimg, never included by `include/fsimg.h` |
| `lib/codec/lzo1x.c` | The LZO1X instruction tables, the decoder state machine and the encoder; one file, because decoder and encoder share the class and distance constants |
| `lib/codec/nrv2b.c` | The NRV2B bit reader, number decoder and one-call decoder |

`lib/codec/` depends on `lib/core/` for statuses, errors and allocation and
on nothing else; nothing in it names a chunk, a header, an image or an I/O
object. `lib/layers/` is its only caller. The directory is always built:
reading the PCM images needs it in the default build. The name means
stream codecs; the `codec.c` files under `lib/formats/` are the formats'
record and integer codecs and stay where they are. Names follow the
style guide: `fsimg_lzo1x_` and `fsimg_nrv2b_` prefixes, `_t` typedefs,
constants in upper case named for their meaning (`LZO1X_MAX_DISTANCE`,
`NRV2B_OFFSET_BONUS_THRESHOLD`), no magic numbers in the machines.

## Clean-room procedure

The codecs are written without reading any codec implementation, so that
nothing in `lib/codec/` is a derivative of GPL or other third-party code.
The procedure and its inputs:

* **LZO1X.** The factual source is the Linux kernel's
  `Documentation/staging/lzo.rst`, documentation prose by Willy Tarreau
  (2014) and Dave Rodgman (2018) describing the stream the kernel's safe
  decompressor accepts. The file carries no SPDX tag, so the kernel's
  default license, GPL-2.0, applies to its text. The copy paraphrased here
  is the one in the kernel's master tree as last changed by commit
  `6df140b2b156b0f7cda62ac93d50de7f8acd2d9b`, fetched on 2026-10-20. This
  design paraphrases its facts into its own tables and copies no text
  beyond field names. The document's author warns that it may contain mistakes;
  the ones found (the first-byte ranges) are corrected above from the
  oracle.
* **NRV2B.** No permissively licensed format description exists. A
  separate describer read the UCL data compression library, version 1.03
  (2004), by Markus F.X.J. Oberhumer, licensed GPL-2.0-or-later, downloaded
  from oberhumer.com (the describer's note records the site, not a fuller
  URL) and read without building in the describer's own scratch
  directory: the NRV2B decoder source, the bit-buffer access header, the
  shared NRV2 encoder source with its bit-writer helper, and the public
  error-code definitions. The describer consulted no UPX or other decoder,
  checked each fact it marks as checked against the oracle's `ucl` and
  `uclenc` commands, listed the facts it could not check, and wrote a
  facts-only note: bit order, the number code, the offset and length rules
  with every constant, the end marker, the copy rule, the error conditions
  and worked examples, with no code and no code-shaped pseudocode. The
  note is not part of the repository; this design carries its facts.
* **Oracle.** `tools/qnx/oracle/oracle.c` is a wrapper that calls
  `lzo1x_decompress_safe`, `ucl_nrv2b_decompress_safe_8` and the matching
  compressors through the libraries' public headers and knows the boot
  image chunk framing. It is built outside the tree against Homebrew's
  `lzo` 2.10 and `ucl` 1.03, both GPL-2.0-or-later, into
  `~/.local/share/fsimg-oracles/ifs-codecs/` and is used only as a binary. Its wrapper may be read; the libraries it
  links may not.
* **This design.** Its author read the two documents above and the oracle
  wrapper, ran the oracle on hand-built streams and on the corpus, and
  read no codec source.
* **Implementers.** Whoever writes `lib/codec/` works from this design and
  the tests alone and reads no codec source: not `liblzo2`, `minilzo`,
  `libucl`, UPX, the kernel's `lzo1x_decompress_safe.c`, nor the owner's
  `~/pcm31/tools/ifs.py`. When agents do the work, describer and
  implementer are separate sessions sharing nothing beyond the accepted
  design. The implementer may run the oracle.
* **Provenance review.** Before the codecs land, a reviewer who did not
  write them confirms that every file carries the BSD-2-Clause SPDX header
  and fsimg copyright; that the structure is this design's LZO1X state
  machine and one-call NRV2B decoder and not a transcription of any
  decoder's control flow; that the only
  literal constants are those the format tables require; that no file in
  the directory's history ever held imported text; and that the commit
  message lists the sources above. The review is recorded with the stage's
  code review.

## Tests

Every passing case is paired with a seeded failure that proves the code
under test ran. The unit tests build every stream by hand from the tables
in this design, in test code that shares nothing with `lib/codec/`.

| Area | Passing cases | Seeded failures |
| --- | --- | --- |
| LZO1X instruction forms (`test/codec.c`) | First runs of 1, 3, 4, 238 and 239 literals; long runs with 0, 1 and 2 extension zero bytes; short matches after state 1, 2 and 3 at distances 1 and 1024; mid-distance short matches after state 4 at 2049 and 3072; near matches of 3, 4, 5 and 8 at distances 1 and 2048; long matches of 3, 33, 34 and 289 at 1 and 16384; far matches of 3, 9, 10 and 265 at 16385 and 49151; 0 to 3 carried literals after each class; the end marker canonical, with `S = 1`, and with `L = 0` and an extension; a stream whose output already equals the limit followed by `11 00 00`, and the same followed by `10 01 00 00`, both `done`; the worked streams above; each decodes to the bytes the oracle produced | Each stream mutated once: distance raised by one past the bytes produced (`FSIMG_ECORRUPT` naming the distance), a near match in place of the marker (unterminated), one byte appended after the marker (trailing), a far match with `H = 1` where the marker was expected; each names its check and the offset |
| NRV2B forms | Examples A to F; reuse before any new offset; `H = 3` with `L` read at a control-byte boundary; `F = 1, 2, 3` and `G = 2, 3, 63`; offsets 0xD00 and 0xD01 with the bonus; offset equal to the bytes produced; `H = 0x1000002` with `L = 0xFF`; the last control byte's unused bits set to ones; the empty stream | Offset one past the bytes produced; a reuse at the start; `H = 0x1000003` (offset out of range); `H = 0x1000002` with `L = 0xFE` (distance before the output start); the marker's `0xFF` dropped (unterminated); a byte after it (trailing); `in_len` of 65536 and `capacity` of 65537 (`FSIMG_EINVAL`) |
| Truncation at every byte | Every proper prefix of every valid stream is unterminated: NRV2B in its one call, LZO1X with `final`; an LZO1X prefix with `final` clear returns `FSIMG_OK` and later decodes to the full output when the rest is presented; the output beyond `produced` keeps its poison in every case. A stream longer than 4096 bytes, whose length comes from the literals that give its matches room, is cut at a stride of points and at every point of its last 64 bytes, where the instructions under test are | The complete stream through the same loop is `done` with the expected bytes, so a loop that ignored `final` would pass a prefix and break the pairing; the poison check is proven live by a deliberately written byte it must report |
| LZO1X split invariance | Every valid LZO1X stream decoded in one call, with every input split point, with output capacity 1, and with history exactly 49151 produces identical bytes and the same `consumed`, `produced` sums and `done` | A history of 49150 for a stream whose last match reaches 49151 back gives `FSIMG_ESTATE`, not `FSIMG_ECORRUPT`, proving the two verdicts are told apart |
| Bounds | A stream whose output is exactly the limit or capacity decodes, end marker included; one byte more is corrupt the moment the length is known (for an LZO1X far match, once its distance word shows it is not the marker), with `produced` unchanged; an LZO1X window of `n - 1` stops with `produced = n - 1`, not `done`, and nothing written beyond it; a long match of 2^32 - 1 claimed under a limit of 65536 is refused at the header with work of a few bytes; a far match whose extension passes 2^32 - 1 is refused before its distance word; a run of 1000 extension zero bytes claiming 255000 literals is refused likewise | The limit raised to cover the claim makes the same stream decode until its input runs out, proving the limit was the operative check |
| Decompression-bomb ratio | A 20-byte NRV2B stream claiming 64 KiB within a 64 KiB capacity decodes in one call, and a 265-byte LZO1X stream (one literal, then a long match of 65535 at distance 1, then the end marker) claiming 64 KiB within a 64 KiB limit decodes in calls of 4 KiB windows; the charged work equals consumed plus produced and the elapsed steps are below a fixed multiple of it | The capacity or limit lowered to 32 KiB refuses the same streams at the length field, before any byte of the match is copied: `produced` is the one literal, and the work is the few bytes of the header |
| Encoder round trip | Blocks of zeros, random bytes, text, period-2 and period-49152 patterns, boundary cases at distances 2048, 2049, 16384, 16385 and 49151, and every 64 KiB slice of each decoded native ImageFS available through `FSIMG_IFS_NATIVE`; each decodes to its input under `fsimg_lzo1x_decode`, and its length never exceeds the bound | A byte flipped in an encoded stream fails the round trip; a capacity one below the bound is `FSIMG_EINVAL` before any write |
| Encoder determinism | A block encoded on a fresh encoder, again after other blocks on the same encoder, in two processes, and under the ASan build with allocation poisoning gives identical bytes | Two blocks are first encoded to plant table entries that a missing check would turn into matches: one leaves, in the bucket of a four-byte group `G` that the block under test contains, the position of a later occurrence of `G`, so the candidate lies after the current position; the other leaves the position of a group that collides with `G` in the hash (found by the test from the hash in this design) but differs in bytes, so the candidate lies before the current position with wrong bytes. The block under test then encodes to the fresh stream; the test documents that dropping the position check or the byte comparison would make it differ |
| Fuzzing (`fuzz/codec.c`) | The input's first bytes choose the codec, the limit or capacity and, for LZO1X, the window sizes; the rest is the stream, decoded in one call and, for LZO1X, again at random split points under ASan and UBSan; the target asserts LZO1X split invariance, progress on every call, no consumption after `done`, and no byte beyond the buffers | Corpus seeded from every stream above and from chunks of the native images |
| Differential checker (`test/codec-integration.sh`) | When `~/.local/share/fsimg-oracles/ifs-codecs/oracle` exists: random byte strings, mutated valid streams and corpus chunks decode under `test/codec --decode` and the oracle with the same accept-or-reject result and, on accept, identical bytes, at a capacity above any decoded size so the reference's off-by-one never applies; error codes are never compared; every encoder stream decodes to its input under `oracle lzo`; a missing oracle is reported as a skip, never a pass | A stream the script knows the oracle rejects, listed as accepted in its self-check table, fails the run |
| Native gates | Every chunk of the eight S1 boot images decodes to the bytes of its `dumpifs -u` copy, or where the serial transport cannot carry that copy, to the oracle's decode whose additive checksum is zero and whose length equals `imagefs_size`; `PCM3_IFS2.ifs`'s stream decodes to the oracle's 68849668 bytes, consumed exactly; every S2 chunk decodes under NRV2B to the `dumpifs -u` bytes (`qnxbase.ifs`: POSIX `cksum` 2731401004 over 3296204 bytes) | A chunk with one byte flipped fails to decode or to match; the images are licensed and reached only through `FSIMG_IFS_IMAGES`, the directory of the nine H3 files and three X1 files under their original names, never from the repository; the native captures come from `FSIMG_IFS_NATIVE` |

## Open decisions

1. **Compression ratio.** Greedy matching with one candidate per bucket
   compresses less than `mkifs`. A wrapped image that must fit its
   template's slot could need a better ratio. Recommendation: keep the
   greedy encoder, measure wrap sizes against the templates at the wrap's
   native gate, and consider lazy matching or a second candidate only if an
   image fails to fit.
