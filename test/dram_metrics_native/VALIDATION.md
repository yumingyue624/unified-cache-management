# DramStore metrics validation — 2026-09-09

Base: `upstream/develop`, commit `f3dd330ed7353f7552aaa1b45a85c5e1f7d4e153`.
Delivery branch: `feat/dramstore-metrics` in
`D:\workspace\unified-cache-management_0`. The implementation was initially
developed and tested in `D:\workspace\dramstore-resource-metrics`, then copied
to the delivery branch without changes to production behavior.

The Ascend container skill was used to test an archive of that base with the
uncommitted implementation overlaid in an isolated container directory:

- Host: `110.138.0.3`
- Container: `codex_drampool_cann851`
- Checkout: `/home/codex/dramstore-metrics-20260909`
- CANN, HIXL and HCCL: 8.5.1; Python: 3.11.14; architecture: aarch64
- Existing cached source dependencies were linked into this checkout's vendor
  directory; no shared source checkout was reset or overwritten.

## Results

| Check | Result |
| --- | --- |
| CANN production targets `dramstore`, `ucmmetrics`, `ucmshared.test`, `ucmstore.test` | Build passed |
| Resource suite using the **production CMake Python extension** | 31 passed after YuanRong-style simplification |
| Existing vLLM connector metrics Python suite | 67 passed |
| `UCMetricsUT.*` | 7 passed |
| NodeActor, NodeScheduler, ReplyService and TaskManager suites | 43 passed |
| Standalone client metrics executable using actual client state machines | Passed |
| ELF dynamic dependencies | DramStore and Python extension both depend on `libmetrics.so` |
| `git diff --check` | Passed |

The resource tests cover interval bucket/count/sum differencing, decrease-based
reset inference, malformed snapshots, partial writes, file rotation,
replayed deltas after state-write or scalar-import failures, atomic rejection of invalid native
batches, overflow, concurrent observations/import/drain, cross-process `flock`,
thread shutdown and one-shot election (losers exit without takeover), role/enable
guards and real Prometheus exposition. There is no instance/sequence tracking or
in-memory retry baseline.

The feature scope was reduced to reuse existing modules: MetricsDispatcher is
identical to upstream/develop; existing vLLM Counter/Gauge merge semantics,
Histogram validation, TLS memory ordering and drain locking remain unchanged.
The shared metrics changes only add the new Histogram import API and binding.
The vLLM metrics adapter only adds DramPool source metadata and labels.
The Scheduler metrics patch and its existing regression test are restored to
upstream/develop; this feature adds no Scheduler/Worker collection merge.

The existing connector suite uses test-only vLLM/native module substitutes. The
resource suite uses the actual C++ metrics implementation and an actual Prometheus
registry, with a minimal vLLM base-class substitute. These are metrics integration
tests, not a running vLLM inference engine.

## Commands

In the isolated checkout after sourcing CANN's `set_env.sh`:

```sh
cmake -S . -B build-cann851 -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_UCM_DPOOL=ON -DBUILD_UNIT_TESTS=ON -DDOWNLOAD_DEPENDENCE=OFF \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DASCEND_ROOT=/usr/local/Ascend/cann-8.5.1 \
  -DHIXL_ROOT=/usr/local/Ascend/cann-8.5.1/aarch64-linux \
  -DPython_ROOT_DIR=/usr/local/python3.11.14
cmake --build build-cann851 \
  --target dramstore ucmmetrics ucmshared.test ucmstore.test -j4
UCM_METRICS_NATIVE_DIR=$PWD/build-cann851/ucm/shared/metrics \
  python3 -m pytest --noconftest test/dram_metrics_native/test_resource_metrics.py -q -o addopts=''
python3 -m pytest --noconftest test/test_ucm_connector_metrics.py -q -o addopts=''
build-cann851/ucm/shared/test/ucmshared.test --gtest_filter='UCMetricsUT.*'
build-cann851/ucm/store/test/ucmstore.test \
  --gtest_filter='UCDramNodeActorTest.*:UCDramNodeSchedulerTest.*:UCDramReplyServiceTest.*:UCDramReplyServicePollingTest.*:UCDramTaskManagerAsyncTest.*'
python3 test/dram_metrics_native/build_client.py g++ ucm/shared/vendor/fmt/include
```

Build/test logs for the reduced scope are in the container at
`/tmp/dram_metrics_scoped_build.log`, `/tmp/dram_metrics_scoped_shared.log` and
`/tmp/dram_metrics_scoped_store.log`.

No DramPool producer code, transport workaround or A2 compatibility patch is
part of this implementation. Real NPU data-transfer E2E and inference-through-HTTP
validation were not run. Producer integration requires the snapshot v1 contract
documented in `docs/source/user-guide/metrics/dramstore_metrics.md`.
