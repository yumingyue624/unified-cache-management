# Codex repository instructions

## Test isolation

- Keep test-only helpers, fakes, stubs, fixtures, test-only constructors, and test-only build support under a `test/` directory.
- Do not add test-only injection paths, switches, or public API parameters to production headers or source files.
- Production code must model only production dependencies and lifecycle. Tests that require real hardware or external services belong to an explicitly opt-in integration-test target.
- A production extension point is acceptable only when it has a concrete runtime use independent of testing; document that use in the code review or task handoff.

## DramPool design authority

- For DramPool work, the repository-root `drampool.md` is authoritative. When it conflicts with other DramPool notes, drafts, or prior generated design documents, follow `drampool.md` and call out the discrepancy.
- Do not add a DramPool launch parameter that is absent from `drampool.md` without explicit user approval.
