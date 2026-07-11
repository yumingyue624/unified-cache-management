# Codex repository instructions

## Test isolation

- Keep test-only helpers, fakes, stubs, fixtures, test-only constructors, and test-only build support under a `test/` directory.
- Do not add test-only injection paths, switches, or public API parameters to production headers or source files.
- Production code must model only production dependencies and lifecycle. Tests that require real hardware or external services belong to an explicitly opt-in integration-test target.
- A production extension point is acceptable only when it has a concrete runtime use independent of testing; document that use in the code review or task handoff.
