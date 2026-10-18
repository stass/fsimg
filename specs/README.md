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
| [Security](design-security.md) | Threat model, extraction, sandboxing, publication, and adversarial tests |
| [Extraction and workers](design-extraction-worker.md) | Safe host output, bounded descriptor traversal, and worker protocol/lifecycle |
| [Filesystem formats](design-formats.md) | Format profiles and interoperability requirements |
| [FAT reader](design-fat-reader.md) | FAT geometry, name profile, traversal, content indexing, and structural verification |
| [FAT writer](design-fat-writer.md) | Deterministic aliases, bounded layout planning, geometry, metadata encoding, and emission |
| [exFAT reader and writer](design-exfat.md) | Boot integrity, bitmap allocation, embedded upcase mapping, initialized data, bounded traversal, and emission |
| [QNX6 reader and writer](design-qnx6.md) | Power-Safe layouts, snapshots, tree addressing, bounded creation/verification, automotive variants, and evidence gates |
| [Command line](design-cli.md) | Commands, flags, defaults, output, and failure behavior |
| [Style](style.md) | C conventions and readability review |
| [Implementation plan](../docs/implementation-plan.md) | Ordered work packages, dependencies, validation, and completion gates |

The principles explain priorities; the architecture owns contracts shared by
multiple subsystems. Detailed specifications own their local contracts. Resolve
contradictions by editing the owning contract and its consumers together, not
by adding exceptions elsewhere. Requirements use present tense. Examples must
obey the specified API and command syntax. Public names use `fsimg_` and C types
use `_t` typedefs.
