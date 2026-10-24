# Typed settings catalogue

## Purpose and boundary

One private, immutable catalogue describes the settings that the command accepts
through `-o` and the corresponding typed library fields. Library option checks,
layer capability reporting, and CLI conversion consume that description. A key
has one spelling, representation, presence rule, and applicability rule across
those consumers. The catalogue is ordinary C data with a small closed set of
value operations; it requires neither a rule engine nor a runtime plugin ABI.

The public API remains the typed structures in `fsimg.h`. Applications initialize
those structures and assign fields; they do not build dictionaries or acquire
catalogue handles. Structure sizes, versions, reserved fields, enum values, and
borrowing contracts remain governed by the [API design](design-api.md). Public
capability records retain their existing representations. The worker job remains
an explicit byte protocol governed by the [CLI design](design-cli.md).

The catalogue describes supplied options, not resolved image plans. A zero chunk
size can mean "use the profile default" without the catalogue knowing that
profile's effective chunk size. Geometry relationships, identifier derivation,
UTF-8 conversion, machine qualification, required boot fields, and input-byte
checks remain with the scheme, container, compression, or filesystem that owns
their meaning. The coordinator never runs those operations.

## Ownership and layout

The private settings module owns `fsimg_setting_t`, its value and syntax enums,
the static catalogue, and operations for lookup, typed access, default testing,
and applicability. Its header is available to libfsimg and the in-tree CLI and
is not installed. Layer and filesystem registries expose only references to
this immutable description; consumers do not copy rows into parallel tables.

Each descriptor contains these facts:

| Fact | Meaning |
| --- | --- |
| Identity | An unqualified key and either a layer owner or filesystem scope; together they produce the canonical CLI key |
| Storage | The option record containing the value, its member offset and capacity, and its exact C value kind |
| Default | The supplied API value meaning default, including nonzero defaults and pointer absence |
| Presence | No separate flag, or the offset of the existing typed `bool` flag |
| Syntax | A closed lexical class, its lexical range or choices, and any CLI-only text capacity |
| Applicability | Read/write use, partition-only use for compose, and the existing filesystem support bit when one applies |
| File identity | For an I/O setting, its existing `fsimg_file_setting_t` identity and section name |

Rows are grouped into immutable layer slices and a filesystem slice. A layer
descriptor references its slice, so its advertised keys and its option checks
have the same membership. The common catalogue view visits these same slices,
including layers absent from a selected stack: it must detect non-default
fields belonging to an absent layer. Stable presentation order is explicit;
file keys precede scalar keys in layer capability output as specified by the
layer API. Lookup is a bounded linear scan of the compiled rows. No hash table,
registration phase, or mutable cache is needed for this small fixed set.

Filesystem rows refer to existing `FSIMG_SETTING_*` bits. A format variant's
`write_settings` mask is its one declaration of support; there is no second
per-format list of setting names. `sector` and `cluster` use the existing
geometry capability fields and driver geometry checks, rather than acquiring
new support bits. `oem` is the read-side filesystem setting. A layer row's owner
is a registry identity, not a repeated test against a string in each consumer.

The catalogue includes all fields subject to shared setting applicability,
even if a particular field has no CLI spelling. Ordinary mode flags, resource
limits, operands, labels, source policy, and publication policy keep their own
interfaces. A shared settings catalogue is not a description of the entire
command or every public option structure.

Member bindings use `offsetof` and `sizeof` of the actual member, with compile
checks tying the member's C type to its value kind. Small initializer macros may
remove repeated member expressions. They do not generate a separate language
or a callback per setting. The finite value-kind switch is the only generic
assignment mechanism. Static rows are trusted build data, checked by compilation
and tests rather than revalidated during each operation. Catalogue tests check key uniqueness, nonoverlapping
member bindings except the declared presence association, owner references,
choice validity, and complete correspondence with public setting masks and
file identities.

## Typed values and defaults

Representation and syntax are different facts. A decimal token does not imply
that the destination is a `uint32_t`, nor does a four-byte member imply that it
is an unsigned integer. Value kinds distinguish `uint32_t`, `uint64_t`, signed
integers when used, `bool`, each concrete public enum type, fixed text arrays,
16-byte identifiers, borrowed text pointers, and borrowed I/O pointers.

Loads and stores use correctly typed temporaries and source-first
`bcopy(src, dst, n)` where an offset addresses a member. Enum storage uses its
actual enum type, never an assumed `int` or `uint32_t` representation. Boolean
stores use a real `bool`; wire bits are decoded into boolean expressions before
assignment. Signed numbers are range-checked as signed values before conversion;
unsigned values are never routed through a signed accumulator. Capacity is a
bound for strings or byte arrays, not a substitute for a scalar's type.

Public option initialization and catalogue default tests agree on these values:

| Value family | API default or absence |
| --- | --- |
| Layer numeric quantities | Zero, requesting the profile's effective default |
| Layer text arrays | Empty string |
| Layer identifiers | Sixteen zero bytes, meaning unset |
| Layer booleans without flags | False |
| DMG and ifsboot compression | `FSIMG_DMG_ZLIB` and `FSIMG_IFSBOOT_LZO` respectively |
| Filesystem byte order | `FSIMG_BYTE_ORDER_LITTLE` |
| Filesystem UUID and inode capacity | Zero bytes and zero respectively |
| Filesystem reserve percentage | 3 |
| Filesystem mountpoint | `NULL`; a pointer to `""` is an explicit empty mountpoint |
| File input | `NULL` |

Default tests compare typed values. They never compare entire native structures
or inspect padding. Empty fixed text is determined by its first character,
after checking termination within capacity. A pointer default is tested as
`NULL`, never by comparing its object representation to zero bytes. Identifier
arrays are byte sequences and may be compared to sixteen zero bytes.

With a separate presence flag, false means absent and requires the associated
value to be its documented inactive value. A nonzero MBR identifier without
`mbr_id_set`, or true `ifsboot_virtual` without its flag, is `FSIMG_EINVAL`.
True means supplied even when the value is zero or false. Consequently a
required address set explicitly to zero satisfies presence, and an explicitly
false boot virtual bit remains distinct from an omitted bit. The profile still
checks required fields and allowed combinations.

Initializers obtain catalogue-owned defaults from the same description used by
default tests. They retain responsibility for structure size/version fields and
ordinary options outside this catalogue. Effective defaults and derived values
are resolved once by their owning planner, without mutating the caller's
options or rewriting an absent flag into an explicit request.

## CLI syntax and explicit requests

The CLI retains `getopt_long(3)`, command-mode validation, stack selection, and
operand handling. After stack and compression selection, it splits each `-o`
argument at the first `=`. For a qualified key it resolves the qualifier and
checks that its owner is selected before consulting the catalogue. On creation,
a selected container or compression without write support takes the deferred
branch described below before key lookup or value conversion. Otherwise the CLI
looks up the exact key. An unqualified key always targets the filesystem. It
never means a container or compression key merely because the same short name
exists there. Lookup is case-sensitive and follows
the spelling in the [CLI](design-cli.md#stack-settings-and-selection) and
[layer settings](design-layers.md#settings) contracts.

The finite syntax classes cover the existing grammar:

| Class | Examples and conversion |
| --- | --- |
| Byte quantity | `sector`, `cluster`, `gpt:disk`, chunk and alignment sizes; checked CLI unit syntax, then destination range |
| Decimal count | `inodes`, compression levels, `ifsboot:machine`; per-key zero allowance and range |
| Hexadecimal 32-bit value | `mbr:id` and boot addresses; `0x` or `0X` and one through eight hexadecimal digits |
| Boolean | Exactly `yes` or `no` |
| Named choice | `endian=little|big`, `dmg:compress=zlib|none`, `ifsboot:compress=lzo|none`, `oem=cp437` |
| UUID | RFC 4122 text converted to textual byte order; nil permitted for the filesystem UUID, refused for layer identifiers |
| Text | GPT type/name, MBR type, mountpoint; checked destination capacity, with format meaning checked by its owner |
| File path | A host operand retained by the CLI for descriptor preparation |

Choice entries pair spellings with values of the row's declared type. Lexical
restrictions and API restrictions are distinct: CLI chunk quantities must be
positive, while an API zero requests a default; `gpt:sector` accepts only 512
or 4096 on the command line while API zero requests detection or the default.
The `oem=cp437` token acknowledges the already documented default read
interpretation and has no CLI destination member: it leaves the initialized
read options unchanged. Its catalogue row is a fixed-choice syntax row without
a storage binding. The public `fat_codepage` API continues to accept zero or
437 and reject unsupported codepages through its read-option validation; this
adds no filesystem qualification or codepage support.

The CLI converts into a temporary typed value and commits it, together with its
presence flag, only after successful conversion. It rejects overflow, trailing
characters, inappropriate signs or units, and text that does not fit. A finite
set of seen catalogue identities detects duplicates on the normal lookup path,
including repeated values that happen to equal defaults. Deferred qualified
keys instead retain their raw key text before `=` for duplicate comparison;
the argument count bounds that storage, and unknown deferred keys need no
catalogue identity. A string's tail is initialized when copied into
a fixed array. Borrowed mountpoint text remains valid until the job is encoded;
the library's ordinary writer construction copies it under its API contract.

Explicit CLI presence and API presence have different purposes:

* A qualified CLI key naming an absent layer fails even when its value equals
  the API default: `simg:crc=no` without simg is an error. On read the layer must
  be explicitly named in the forced stack. Detection cannot supply authority
  for the coordinator to accept the key.
* A write-only key on a read command fails even when its parsed value would be
  default. File settings fail at this step, before any path is opened.
* Unsupported filesystem fields retain the public default exception: a format
  without `FSIMG_SETTING_RESERVE` accepts `reserve=3` and refuses a non-default
  reserve value. Explicitness does not invent a stronger applicability rule.
* A library caller supplies typed values and existing flags only. It is not
  required to provide the CLI's seen set, and a default field without a flag
  carries no recoverable history of whether the caller assigned it.

The layer contract's unsupported-write precedence is preserved: a selected
container or compression with no write support leaves its qualified key and
value uninterpreted until the unsupported layer is rejected. For example, a
selected read-only `hbcifs` with `hbcifs:anything=garbage` fails with
`FSIMG_EUNSUPPORTED`, rather than an unknown-key or malformed-value error.
Duplicate detection still compares the raw qualified key, but the CLI neither
opens a purported file value nor runs a profile validator for that rejected
operation. Scheme settings follow
their normal syntax checks. This exception does not add unknown keys to the
catalogue or advertise them as supported.

## Validation and errors

Validation has three owners with a visible order:

1. The CLI checks trusted command syntax, duplicate keys, mode use, selected
   owners, and lexical conversion. It may inspect immutable descriptors and
   capability records; it does not open image bytes to interpret an option.
2. Shared library checks validate option structure versions and reserved fields,
   bounded fixed text, enum membership, inactive values, and applicability.
   Every applicable entry point uses these checks before dispatching to an
   owner. Writers reject unsupported non-default filesystem settings before
   the format's validation operation. Compose rejects partition settings in
   its outer options; each part supplies its own partition settings. Wrap
   requires filesystem fields to hold their documented defaults.
3. Driver or layer validation checks semantic relationships and resolves
   effective values. Planning validates lengths, contents, encodings, geometry,
   and qualified native combinations before emission, under the usual resource
   and sandbox contracts.

The library repeats structural and applicability checks for decoded jobs and
embedding applications; CLI validation is not a security boundary for libfsimg.
Malformed worker bytes are rejected before they become native typed objects.
No unchecked byte string is read as a native boolean, enum, pointer, or struct.

Unknown keys, inappropriate owners or modes, malformed values, and unsupported
non-default fields are invalid arguments. Library setting failures use
`FSIMG_EINVAL` and name the canonical key in `error.path` where the setting error
contract applies. A recognized but unavailable operation or profile retains
`FSIMG_EUNSUPPORTED`; changing catalogue storage does not turn it into an unknown
name. Content corruption, changed inputs, resource exhaustion, and I/O failures
retain their existing classes. Shared checks report the first failing row in
stable catalogue order; CLI lexical failures follow argument order. Error
messages identify the constraint and bounded key rather than dereferencing or
printing arbitrary option pointers or file contents.

## File inputs and transport

A file row shares key identity, owner, mode, and applicability with other rows.
Its storage binding names the `fsimg_layer_inputs_t` I/O member and its existing
file identity. CLI host paths and library I/O objects remain distinct values:
the path is held in the CLI's file-operand slots, never placed in an I/O pointer
field. The descriptor-preparation path opens a regular read-only file using the
CLI operand protections and passes the descriptor under its existing capability
identity. The worker receives no path and wraps the descriptor as bounded I/O.

The catalogue neither opens nor closes an input. It does not own buffers or
change the lifetime stated in
[file settings and sections](design-layers.md#file-settings-and-sections).
Common input handling owns identity capture, alias rejection, and change
checks. The container profile owns whether its input is bounded or streamed,
its size/content rules, and padding. Section enumeration uses the file rows'
existing names, so adding a file setting does not require a separate section
name table. An absent pointer and a present empty input remain distinguishable;
only the profile decides what an empty input means.

`cmd/fsimg/job.c` owns the wire mapping. Native catalogue offsets are never wire
offsets, and native enum width or pointer representation never determines a
frame. Numeric widths, little-endian encoding, known flag bits, zero padding,
terminated text, optional mountpoint presence, boot presence bits, and separate
file descriptors retain their explicit representation. Decoding validates wide
wire integers before narrowing and validates flags before forming enums or
booleans. Encoding initializes every emitted byte.

The codec may use catalogue identities in coverage tests and obtain field
values through typed access, but it retains explicit encoding and decoding.
There is no generated wire schema in this design. CLI-only explicitness does
not add a presence bitmap to the job: mode and owner checks occur before
encoding, while existing API presence flags travel in their existing bits.
A new transported setting requires an explicit protocol decision and coverage;
adding a catalogue row cannot silently change the record layout.

## Compatibility and verification

The catalogue preserves public option layouts, initializer defaults, key
spellings, accepted syntax, capability membership and ordering, error classes,
wire bytes, and output image bytes. It does not broaden a format's support
because another format shares a key. Static syntax facts can be shared without
sharing format-specific planning, and no Relic or other generator is needed for
normal builds or development of these tables.

Tests demonstrate the consumer agreement without deriving every expected answer
from the catalogue under test:

* Independently written CLI cases cover every key family, each named choice,
  duplicate keys, wrong modes and layers, forced read stacks, unqualified
  filesystem keys, unit parsing, overflow, signs, trailing text, and capacities.
* Library cases cover initialized defaults and non-default unsupported fields,
  reserve 3 versus 0, empty versus NULL mountpoint, nil UUID distinctions,
  invalid enum values, and terminated versus unterminated fixed strings.
* Presence cases cover each flag with zero and nonzero values, unset flags with
  nonzero values, false versus absent boot virtual, required absent boot
  fields, and file absence versus a zero-length file.
* Applicability cases cover create, read, wrap, compose outer options and parts,
  selected compression, unavailable layer write support, and rejection before
  opening file operands or touching a sink where those contracts require it.
* Catalogue consistency cases check registry ownership, public mask coverage,
  file identity coverage, presentation capacity and stable ordering. No
  capability key is silently truncated when appended to a fixed public field.
* Known wire vectors and decode/encode tests cover every transported member,
  its non-default value and existing presence flag, zero padding, malformed
  widths and flags, and optional strings. They compare with explicit expected
  bytes, not another codec generated from the same descriptor.
* Paired CLI and library operations produce the same effective options and
  image bytes on representative stacks. Existing independent-tool checks
  establish format compatibility; shared metadata alone is not an oracle.

A maintainer adding a setting edits its one catalogue row, its owning format
interpretation, and its explicit transport mapping when required. Supporting a
filesystem setting in another variant changes that variant's existing support
mask; supporting a layer setting changes the layer's referenced catalogue slice.
Consumers acquire neither new key switches nor new per-field default tests.
The retained responsibilities are deliberate: public typed layout, format
semantics, command syntax outside `-o`, and protocol bytes have different owners
and compatibility contracts.
