# Prepared directories and common traversal

Every reader exposes a volume as a tree of directories whose children are
listed in on-disk order and whose names resolve unambiguously. This document
defines one contract for that: a driver prepares a directory, and a common
walker owns everything that happens after preparation succeeds. Drivers learn
nothing about stacks, paths, pruning, or selection; the walker learns nothing
about directory encodings, allocation, or name folding.

The [architecture](architecture.md#writer-and-reader-independence) assigns the
owners. [Storage](design-storage.md#reading-and-traversal) states the bounds.
Driver designs ([FAT](design-fat-reader.md), [exFAT](design-exfat.md),
[QNX6](design-qnx6.md)) define only their decoding, keys, and locators.
Host source scanning and safe extraction have separate authority and lifetime
rules and do not use this walker.

## Owners

| Owner | Owns | Does not own |
| --- | --- | --- |
| Driver | Decoding directory storage, entry framing and metadata, child allocation and parent references, directory-cycle claims, comparison keys, private locators | Traversal order, ancestor state, paths, depth/path/entry limits, selection |
| Preparation | Admitting entries: charging them, enforcing depth and path limits, spooling records, validating the namespace, sealing the prepared directory | Interpreting record payloads |
| Walker | Depth-first order, deferred descent, pruning, path assembly, ancestor cursors, directory-stream lifetime, the current-entry snapshot | Name comparison or any format field |
| Selector | Resolving an image path one component at a time and choosing the traversal scope | Directory decoding or private namespace indexes |

Preparation, walker, and selector are common reader code in `lib/read/`.
Structural verification iterates through the same walker; its ownership
claims are emitted by the driver while preparing directories and resolving
file allocation. It has no second traversal engine.

## Prepared directory

A prepared directory satisfies one invariant:

> Its children have valid framing and metadata, its lookup namespace is
> unambiguous, and its entries can be replayed in original order.

It consists of a sealed, spillable stream of entry records in on-disk order.
Each record holds the displayed component as validated UTF-8 and a bounded,
explicitly encoded driver payload. The payload carries what the driver needs to
rebuild the entry snapshot and its private locator: for example, the FAT
32-byte short entry, or exFAT stream fields. Records never contain native
structures, pointers, or padding.

An incomplete directory is private to its preparation. The walker receives a
directory only after namespace validation and sealing succeed, and owns its
lifetime from then on. No caller can observe a child whose siblings have not
been validated, so namespace validation is a property of every exposed
directory, not a step each traversal path must remember.

Preparation does not make scratch bytes permanently trustworthy. Replay
re-checks record framing and lengths, and the driver re-validates the decoded
payload fields and locator before they become the current entry.

## Namespace

The shared invariant is:

> Multiple spellings may identify one directory entry. One comparison key
> cannot identify two different directory entries.

While decoding, the driver emits one or more comparison keys for each admitted
entry. FAT emits its long-name key and alias key; exFAT emits the name mapped
through the volume's validated upcase table; QNX6 emits the case-sensitive UTF-8
bytes. Keys are opaque, bounded byte strings to common code. Preparation stores
key records `(key, entry ordinal)` in a second stream, sorts them externally,
and rejects equal keys with different ordinals. Duplicate keys for the same
ordinal are permitted. The key stream is released when validation completes.
The driver derives an entry's keys with one function from its entry record,
the displayed name and payload. Preparation and selection both call it,
selection on the replayed and re-validated record.

Entry identity is the entry's ordinal within the prepared directory, a
directory-entry occurrence. It is never inode or allocation identity: separate
directory entries can legitimately reference one object, and that never permits
two names to share a key within a directory. Allocation-owner identity belongs
to [ownership verification](design-storage.md#verification-scope).

The driver supplies the error classification and diagnostic text for an
ambiguous namespace. Common code identifies the colliding entries by their
validated displayed names, never by raw key bytes.

## Selection

A query component matches an entry when the driver's key for the query equals
one of that entry's namespace keys. The driver produces both keys; the selector
compares bytes. Selection therefore uses exactly the keys that namespace
validation proved unambiguous, so a lookup cannot reach an entry the
validation did not examine and cannot pick the first of two equivalent names.
A component that the driver cannot represent is an invalid query, not a miss.

Resolved paths retain displayed names. Off-branch subdirectories are pruned
after their parent is prepared, without being opened. Selecting one child still
requires the parent's complete namespace validation.

## Traversal

The walker performs iterative depth-first traversal. Its ancestor stack is
bounded by the configured depth limit, and each level retains only its
prepared-directory stream, replay cursor, and path length. One charged path
buffer holds every ancestor prefix. A directory entry is returned before its
children. Descent is deferred to the next advance, so the caller can prune the
subtree first. A directory's scratch is released as soon as its stream is
exhausted. Retained ancestor streams share scratch descriptors, so descriptor
use does not grow with depth. A legal tree within the depth limit cannot
exhaust the descriptor limit.

The driver opens a child directory from the current entry's locator, and claims
its storage there. For FAT and exFAT that claim uses a paged exact bitset, so
re-entering claimed storage is corruption whether it comes from a cycle or a
shared chain. A driver with different object identity supplies an equally
bounded exact scheme.

## Charging and limits

Preparation charges each entry against the session when it admits the entry,
and enforces the depth and complete-path limits at the same point. Replay does
not charge the same entry again, and it cannot exceed a limit that admission
already enforced. Decoding, cache misses, and sorting consume work units in the
ordinary way. The first advance into a wide directory can therefore do work
proportional to its width, but opening the volume stays bounded and no
whole-volume tree is materialized.

## Snapshots and borrowing

The current entry snapshot, its path, and the driver's current locator are
borrowed. They are valid until the next advance begins, a terminal failure, or
reader destruction. A retained ancestor cursor never borrows the record buffer
that the next replay overwrites. Content state for the current file belongs to
[logical content](design-content.md) and is discarded when the walker advances.

## Failure

Argument and state errors detected before work leave the reader unchanged. A
failure in preparation, replay, a driver callback, I/O, a resource limit,
cancellation, or a source change is terminal. The walker invalidates the
snapshot and releases every prepared directory it holds. A partially prepared
directory is released by preparation before the error is returned. Driver
cleanup accepts partial construction and repeated calls.
