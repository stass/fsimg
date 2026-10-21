# Library examples

The checked programs in `examples/` use only the installed public API. They
build with the ordinary project and run as part of CLI integration testing.
They do not sandbox their embedding process; use the `fsimg` command when the
same-executable worker and OS confinement policy are required.

Use stable sources. The host scanner detects metadata-visible changes, but
coarse timestamps and hostile metadata-preserving mutation require immutable
inputs or a filesystem snapshot. Provider state and content stay borrowed until
writer destruction. Callback source and sink storage must be disjoint.

From a configured build directory, these commands create disposable fixtures:

```sh
mkdir example-source example-output
printf 'example content\n' > example-source/content.txt
scratch=$(mktemp -d)
./example-create-tree example-source example-output tree.img "$scratch"
./example-provider example-output provider.img "$scratch"
length=$(wc -c < example-output/provider.img | tr -d '[:space:]')
./example-read-slice example-output/provider.img 0 "$length" "$scratch"
./example-extract example-output/provider.img example-output extracted "$scratch"
rmdir "$scratch"
```

The scratch directory must be private and disjoint from the source and output
trees. Metadata can spill there; payloads are never staged in scratch. These
small examples choose a 16 MiB library budget. A caller can select different
memory, scratch, traversal and logical-output limits with initialized options.

`create-tree.c` imports regular files and directories and publishes a new exFAT
image without replacement. `provider.c` demonstrates immutable callback content
and short successful reads. `read-slice.c` checks a byte range before opening a
reader, enumerates entries and reads bounded content samples. `extract.c` uses
the default safe extraction profile with no timestamp restoration. Inspection
metadata has validity bits, timestamp basis and resolution; it does not imply
POSIX ownership/mode fidelity.

Successful header opening, enumeration, sampled content, complete structural
verification and complete payload read-through are distinct results. A confirmed
prefix accompanying a read error is incomplete. Raw writer sinks can retain
partial bytes. The publication helper instead reports an absent, published or
uncertain final output, plus retained staging and cleanup failures. Do not retry
publication blindly after a published or uncertain result. Retained staging
paths in library errors are relative to the supplied output parent.

Extraction creates one fresh private directory and never merges or overwrites.
Failure after creation can leave incomplete content there. The example reports
that outcome and leaves recovery decisions to its caller. Library cleanup never
recursively follows image-derived paths.

To build an example against an installed shared library:

```sh
cc -std=c11 $(pkg-config --cflags libfsimg) -Iexamples \
    examples/provider.c $(pkg-config --libs libfsimg) -o provider
```

The same public header works from C++. The installed static archive can be
linked directly with the host compiler, together with the system zlib
(`-lz`), which `libfsimg.pc` names. No external filesystem tool or other
third-party runtime library is required.
