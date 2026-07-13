# Codex repository instructions

## Test isolation

- Keep test-only helpers, fakes, stubs, fixtures, test-only constructors, and test-only build support under a `test/` directory.
- Do not add test-only injection paths, switches, or public API parameters to production headers or source files.
- Production code must model only production dependencies and lifecycle. Tests that require real hardware or external services belong to an explicitly opt-in integration-test target.
- A production extension point is acceptable only when it has a concrete runtime use independent of testing; document that use in the code review or task handoff.

## DramPool design authority

- For DramPool work, the repository-root `drampool.md` is authoritative. When it conflicts with other DramPool notes, drafts, or prior generated design documents, follow `drampool.md` and call out the discrepancy.
- Do not add a DramPool launch parameter that is absent from `drampool.md` without explicit user approval.

## DramPool process model

- Each compute node starts one independent DramPool process. A DramPool process supports one `DramPoolServer` instance only.
- `g_config` is the sole process-wide DramPool configuration. In production it is populated by `ParseCommandLine()` before `DramPoolServer::Init()` and is read-only thereafter.
- Do not add per-server configuration copies, configuration injection paths, or multi-server support unless the user explicitly changes this architecture.
- `ParseCommandLine()` performs the complete launch-configuration validation. Do not duplicate that same validation in `DramPoolServer::Init()`.
- `DramPoolServer` owns all mutable runtime components. `DramPoolRuntime` is its non-owning internal context and is passed by reference to Worker and Poller; do not add a global service locator such as `g_services`.
- `Init()` creates local memory, metadata, protocol, queues, `TransportManager`, and `DramPoolRuntime`, but must not open a TCP listener. `Start()` starts internal workers and the Receiver, then starts the transport service and its TCP listener last.
