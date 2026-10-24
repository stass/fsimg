# fsimg specifications

These documents define the target behavior and architecture of **fsimg**, the
filesystem image tool and C library. They are the authoritative specification.
Implementation status and sequencing belong in the implementation plan and
`TODO.md`, not in these specifications.

| Document | Responsibility |
| --- | --- |
| [Principles](principles.md) | Concise rules for choosing between designs |
| [Architecture](architecture.md) | Product scope, subsystem boundaries, shared contracts, resource budgets, and acceptance criteria |
| [C API](design-api.md) | Types, operations, ownership, state transitions, and errors |
| [Storage and algorithms](design-storage.md) | Bounded I/O, metadata scratch storage, planning, and complexity bounds for reading, verification, and emission |
| [Sources and catalogs](design-sources.md) | Source identity, scanning/reopening, metadata encoding, deterministic catalog preparation |
| [Prepared directories](design-directories.md) | Directory preparation, namespace keys, common traversal and selection |
| [Logical content](design-content.md) | Logical, initialized and mapped content; common delivery; optional extent index |
| [Emission](design-emission.md) | Emission order, sink initialization, emission object, complete source copies |
| [Settings catalogue](design-settings.md) | Shared typed settings descriptions, applicability, CLI conversion, file settings, and the explicit worker transport boundary |
| [Writer metadata losses](design-metadata-loss.md) | Common refusal and accounting policy, planning callbacks, and narrow timestamp conversions |
| [Container plans](design-container-plans.md) | Private immutable container state, common planning facts, encoder borrowing, and complete destruction |
| [Hole knowledge](design-holes.md) | The written-range record of a fresh file, the zero-range operation every I/O object answers, and the reads a transcode may spare |
| [Automatic sizing](design-sizing.md) | The default first-fit sizing policy and the minimization option, per format |
| [Security](design-security.md) | Threat model, extraction, sandboxing, publication, and adversarial tests |
| [Extraction and workers](design-extraction-worker.md) | Safe host output, bounded descriptor traversal, and worker protocol/lifecycle |
| [Filesystem formats](design-formats.md) | Format profiles and interoperability requirements |
| [FAT reader](design-fat-reader.md) | FAT geometry, name profile, traversal, content indexing, and structural verification |
| [FAT writer](design-fat-writer.md) | Deterministic aliases, bounded layout planning, geometry, metadata encoding, and emission |
| [exFAT reader and writer](design-exfat.md) | Boot integrity, bitmap allocation, embedded upcase mapping, initialized data, bounded traversal, and emission |
| [QNX6 reader and writer](design-qnx6.md) | Power-Safe layouts, snapshots, tree addressing, bounded creation/verification, automotive variants, and evidence gates |
| [QNX6 driver](design-qnx6-driver.md) | Module layout, driver state, tree walker, bitmap self-check, reservation domains, planning passes, emission order, and tests |
| [QNX4 reader and writer](design-qnx4.md) | Derived geometry, the root block, inode, link, extent-block and long-name records, closed status tables, format-owned files, the native default layout the writer reproduces, and the PCM navigation partition |
| [QNX4 driver](design-qnx4-driver.md) | Module layout, probe and open order, the one extent walker, bitmap pager, link resolution, verification, planning passes, emission order, and tests |
| [QNX ImageFS reader and writer](design-ifs.md) | The flat header and record table, the table and range rules, kinds and the one symbolic-link layout, inode flags and boot references, the additive checksum, the hierarchy derived from sorted paths with shadowed duplicates and synthesized parents, closed acceptance tables, the writer's one deterministic layout, and the PCM and live-CD bodies |
| [QNX ImageFS driver](design-ifs-driver.md) | Module layout, probe and open order, the single table pass, the two-sort hierarchy derivation feeding the common walker, content, verification, writer settings, planning passes, emission with the running word sum, and tests |
| [Compression codecs](design-codecs.md) | LZO1X and NRV2B decoders and the LZO1X encoder |
| [POSIX metadata](design-metadata.md) | Entry kinds beyond files and directories; owner, mode and change time across API, catalog, scanner, extractor and CLI |
| [Manifests](design-mtree.md) | Creating from an mtree(5) manifest as a source root: grammar subset, generated content, host bytes beneath a content directory, keyword mapping, bounds; the manifest extraction writes |
| [Layers](design-layers.md) | Partition schemes, disk-image containers, compression, spooling and composition |
| [Command line](design-cli.md) | Modes, bundled letters, options, stacks and settings, operands, destinations, output, and failure behavior |
| [Specification-driven testing](design-spec-testing.md) | Rule-derived valid and invalid images, per-stage expectations, the CLI runner contract, fixtures and coverage reports |
| [Style](style.md) | C conventions and readability review |
| [Implementation plan](../docs/implementation-plan.md) | Ordered work packages, dependencies, validation, and completion gates |

The principles explain priorities; the architecture owns contracts shared by
multiple subsystems. Detailed specifications own their local contracts. Resolve
contradictions by editing the owning contract and its consumers together, not
by adding exceptions elsewhere. Requirements use present tense. Examples must
obey the specified API and command syntax. Public names use `fsimg_` and C types
use `_t` typedefs.
