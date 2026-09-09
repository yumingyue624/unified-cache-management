# DramStore and local DramPool metrics

DramStore records client task/request counts, client-accepted entry results and
latencies in the existing UCM metrics library. A Python Reporter thread in the
Scheduler process reads the local DramPool's cumulative JSON Lines snapshots.
Counter and Histogram deltas enter the same library and leave through vLLM's
existing connector Prometheus endpoint. Device workers do not start a file reader.
The Reporter keeps reading while idle, but publication still depends on vLLM
calling the connector metrics hook; no independent HTTP exporter is started.

The existing MetricsDispatcher drain/distribution and vLLM Histogram aggregation
are reused unchanged. The vLLM metrics adapter only gains the DramPool source label;
existing Counter/Gauge merge rules and validation remain unchanged. Snapshot
validation belongs to the new reader and the new native Histogram import API.
Scheduler and Worker processes retain their existing collection and reporting
paths. This feature does not change Scheduler collection conditions or add a
Scheduler/Worker merge; refresh timing remains that of the existing vLLM hooks.

## Enable the file reader

Add these fields to the Dram store connector configuration:

```yaml
drampool_resource_metrics_enable: true
drampool_resource_log_path: /mnt/drampool-logs/drampool_metrics.log
drampool_resource_shared_dir: /mnt/drampool-metrics-state
```

The source endpoint must appear in the existing `node_control_endpoints` list.
These are UCM client settings; no DramPool launch parameters are added. The
producer implementation is separate from this change.

The log file must refer to the local host's DramPool. All candidate Schedulers
must see the same log and the same writable, existing state directory. Container
private `/tmp` or `/dev/shm` directories do not provide host-wide election. The
reader does not create or fall back to a different shared directory. `flock`
selects one reporter at thread startup; losing candidates exit immediately.
Existing candidates do not take over after the leader exits. Only newly started
reporters attempt election again. Different log
mount paths for the same source share the same lock identity.

UCM metrics must be enabled and the `vllm_connector` consumer selected. The
vLLM launch-wide metrics switch and definitions are passed into the nested Dram
store configuration, so `enable_metrics: false` also disables this reader. The
default metric definitions include the initial DramStore/DramPool families.
Custom `metrics_config` / `metrics_config_path` acts as a whitelist; include the
source metrics and `drampool_resource_*` health metrics you need. Histogram bucket
definitions must match the producer. No names are registered from untrusted file
contents. An older native binding without `merge_histogram_stats` disables the
file reader with a warning; business operations continue.

## Snapshot v1 contract

Each UTF-8 line ends in a newline and contains an independent, full snapshot:

```json
{
  "event": "drampool_metrics_snapshot",
  "schema_version": "v1",
  "source_id": "10.0.0.1:12345",
  "timestamp": "2026-09-09T10:00:00+08:00",
  "counters": {"drampool_load_requests_total": 36},
  "gauges": {"drampool_capacity_bytes": 8192, "drampool_used_bytes": 4096, "drampool_available_bytes": 4096},
  "histograms": {
    "drampool_load_duration_us": {
      "unit": "us",
      "upper_bounds": [100, 500, 1000, 5000, 10000, 50000, 100000, 500000, 1000000, 5000000, 10000000, 30000000],
      "bucket_counts": [10, 20, 5, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0],
      "count": 36,
      "sum": 12000
    }
  }
}
```

The example is expanded for readability; the actual record occupies one line.
Timestamp may also be nonnegative epoch seconds. ISO timestamps require a timezone.
`source_id` is the canonical configured DramPool endpoint, not a container path or
the client's control address. Counter and Histogram values are cumulative for
one producer process. Histogram boundaries and units remain fixed. No instance ID
or sequence number is required. Keep the same enabled fields in every snapshot.
New fields in an existing state are differenced against zero, as in YuanRong;
missing Gauge values are not converted to zero.

Histogram buckets are **disjoint interval counts**, not cumulative Prometheus
`le` buckets. The last count is the implicit `+Inf` interval, so there is one more
count than finite upper bound. `count` must equal the sum of the counts; `sum` is
the sum of actual durations in microseconds. Precomputed percentiles cannot be
subtracted or merged and are not used as Histogram observations.

For example, with bounds `[100,500,1000]`, cumulative counts changing from
`[10,20,5,1]` to `[12,23,6,1]` and sum changing from `12000` to `13900` produce
`([2,3,1,0],1900)` at the native import interface. The final Prometheus bucket
increments are `[2,5,6,6]`, count increments by 6, and sum increments by 0.0019
seconds. Intermediate UCM layers do not compute a prefix sum.

The reader scans at most 2 MiB from the file tail, accepts records up to 1 MiB,
ignores incomplete trailing lines, and reopens the active path each round for
rotation. Cumulative values must not reset merely because the file rotated.

## Semantics and limitations

- With no valid state, the first Counter/Histogram values form a baseline;
  only Gauges are immediately published. No startup history is backfilled.
- Repeated cumulative values produce zero deltas through subtraction alone.
- A decreasing Counter is treated as a reset and contributes its current value,
  as in YuanRong. If any Histogram bucket or its sum decreases, that entire
  Histogram contributes its current buckets/count/sum. Bucket schemas must match.
- Reset inference cannot detect a restarted producer whose totals already exceed
  the old totals; an older snapshot can also be mistaken for a reset.
- Each round reads the previous baseline from state, imports Histogram and scalar
  deltas, then atomically replaces state. There is no in-memory baseline recovery
  or partial-import retry bookkeeping. Failed state writes can replay deltas next
  round; a scalar failure after Histogram import can replay that Histogram.
- State replacement prevents partial files, not partial delivery. Crashes between
  import, state write, forwarding and scraping may lose or duplicate an interval.
- No new resource Gauge sampler or server-side instrumentation is included.
  Abrupt process exit and tasks abandoned by the existing shutdown lifecycle
  do not produce a guaranteed final metric flush.
- `dramstore_*_acknowledged_bytes_total` measures logically confirmed bytes.
  Duplicate DUMP keys may be successful without data transfer; use the server's
  `drampool_*_bytes_total` for physical data-transfer accounting.
- Task metrics are settled once, independently of repeated `Check`/`Wait`.
  A task can split into multiple node requests and tensor entries. Do not add
  task, request and entry counts together.

Imported metrics have an extra `drampool_endpoint` label. An elected reporter's
exit stops collection until another newly started reporter wins the lock. Existing
losing reporters have already exited and do not retry. If a new reporter later
publishes on another HTTP target, select the newest fresh Gauge per endpoint;
do not sum capacity Gauges across targets. Apply `rate` per Counter series before
aggregating and accept best-effort gaps or duplicates.

## Development validation

CPU-only tests compile the real native metrics binding and client state machines;
test-only logger replacements and dependency helpers stay under `test/`:

```sh
python test/dram_metrics_native/build_binding.py g++
python test/dram_metrics_native/build_client.py g++ /path/to/fmt/include
python -m pytest --noconftest test/dram_metrics_native/test_resource_metrics.py
python -m pytest --noconftest test/test_ucm_connector_metrics.py
```

Run the two Python suites in separate processes because the existing connector
suite installs module-level vLLM/native fakes. `--noconftest` bypasses the unrelated
global GPU resource fixture for these CPU-only suites. The file-lock test needs
POSIX. A real Prometheus registry verifies cumulative buckets and unit conversion;
the test provides only the vLLM base-class interface, not an inference engine.
To test a normal CMake build instead of the standalone binding, set
`UCM_METRICS_NATIVE_DIR=$PWD/build-cann851/ucm/shared/metrics` for the resource suite.

Update `examples/metrics/metrics_configs.yaml` first, then run
`python examples/metrics/generate_dram_defaults.py` to regenerate the matching
Dram metric block in `ucm/default_metrics_config.py`.
