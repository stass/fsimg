# fsimg principles

1. **Security and correctness are inseparable.** Images, names, metadata, and
   source trees are untrusted. Validate before use, grant the least authority,
   and make failure explicit. Sandboxing contains defects; it never replaces
   bounds checks or safe extraction.

2. **Bound resources independently of image size.** Payloads, allocation tables,
   entry catalogs, sorting, traversal, and diagnostics all obey budgets. Spill
   metadata to bounded scratch storage; never silently grow RAM or stage a
   payload image, except through the two explicit, separately budgeted
   files the layer design names. Measure adversarial cases as well as
   ordinary ones.

3. **Plan new images; read existing ones.** A writer turns stable sources into
   a validated layout and then emits it. A reader decodes an existing volume.
   Neither needs a mutable VFS, mounts, or in-place modification.

4. **Make invariants hold at one boundary.** Bounded I/O owns byte ranges,
   the resource context owns budgets, the extractor owns host paths, and the
   output coordinator owns publication. Drivers cannot bypass those owners.

5. **Prefer fewer concepts and obligations.** Judge simplicity across the
   system, not by line count. Use small, explicit data flows and closed state
   machines. Reconsider a design that needs a growing list of cooperating
   callers or exceptions.

6. **Be exact about capabilities and losses.** Reading does not imply writing;
   a recognized filesystem does not imply every feature is supported. Reject
   ambiguous detection, collisions, corruption, and silent metadata loss.
   Report the guarantee actually provided, including sandbox availability.

7. **Make ownership and failure visible.** Interfaces state units, lifetimes,
   borrowing, and cleanup. Prepare before publishing. A partial output or an
   uncertain durability result is never reported as complete success.

8. **Make reproducibility deliberate.** Stable ordering and explicit policies
   govern names, layout, timestamps, identifiers, and padding. Host iteration
   order, locale, scratch placement, and cache size do not change image bytes.

9. **Prove interoperability independently.** Self-round-trips are useful but
   insufficient. Use external implementations, malformed fixtures, fault
   injection, fuzzing, sanitizers, and measured resource bounds.

10. **Keep fsimg small and embeddable.** Use portable C11, BSD-style `_t`
    typedefs, BSD-2-Clause project code, libc/OS interfaces and the system
    zlib, the one third-party library the architecture permits. Keep
    optional integrations outside the core. The library neither exits the
    process nor silently changes its sandbox or global state.
