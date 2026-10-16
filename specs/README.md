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
| [Storage and algorithms](design-storage.md) | Bounded I/O, metadata scratch storage, reading, planning, and emission |
| [Security](design-security.md) | Threat model, extraction, sandboxing, publication, and adversarial tests |
| [Filesystem formats](design-formats.md) | FAT and exFAT profiles and interoperability requirements |
| [Command line](design-cli.md) | Commands, flags, defaults, output, and failure behavior |
| [Style](style.md) | C conventions and readability review |
| [Implementation plan](../docs/implementation-plan.md) | Ordered work packages, dependencies, validation, and completion gates |

The principles explain priorities; the architecture owns contracts shared by
multiple subsystems. Detailed specifications own their local contracts. Resolve
contradictions by editing the owning contract and its consumers together, not
by adding exceptions elsewhere. Requirements use present tense. Examples must
obey the specified API and command syntax. Public names use `fsimg_` and C types
use `_t` typedefs.
