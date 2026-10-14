# DramPool

[English](drampool.md) | [中文](../../zh/user-guide/prefix-cache/drampool.md)

DramPool pools host DRAM for KV cache reuse across inference instances. DramStore is the UCM client that reads and writes KV blocks in the pool through HIXL. Cached blocks expire after a configured lifetime; restarting DramPool clears its in-memory cache.

## Prerequisites

- Ascend A3 or later hardware, with CANN **9.1.0 or later** and compatible drivers and HIXL libraries.
- A Linux environment with RDMA networking between DramPool and inference nodes, and enough available host memory for the configured pool and runtime buffers.
- UCM and a compatible vLLM-Ascend installation. See [Quickstart-vLLM-Ascend](../../getting-started/quickstart_vllm_ascend.md) for inference integration.

## Launch DramPool

### Build

From the UCM repository root, load the CANN environment and build UCM for A3:

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export PLATFORM=ascend-a3
pip install -v -e . --no-build-isolation
```

If HIXL is installed separately, set `HIXL_ROOT` to its installation directory before building and add its library directory to `LD_LIBRARY_PATH` before starting the service. Confirm that the build reports `P2P HIXL transport backend is enabled`. Use the platform setting appropriate to your hardware for later Ascend generations.

### Configure and start

Copy [examples/drampool.yaml](../../../../examples/drampool.yaml) to a runtime configuration file. Set `transport.device_ids` to available device IDs. Configure `transport.endpoints` with the control (`two_sided`) and transport (`one_sided`) addresses of DramPool and all DramStore clients; the DramPool `two_sided` address must match `--addr`.

For example, the DramPool entry can be configured as:

```yaml
transport:
  device_ids: [0]
  hixl:
    listen_port: 26666
    enable_cs: false
  endpoints:
    - two_sided: "192.168.1.10:9000"
      one_sided: "192.168.1.10:4501"
    # Add DramStore client endpoints here.
health:
  port: 9100
```

Merge these settings into the copied file, then start the executable from the repository root (the editable build installs it under `bin/`):

```bash
./bin/drampool \
  --addr 192.168.1.10:9000 \
  --nics mlx5_0 \
  --pool-size-gb 64 \
  --kvcache-block-sizes 1048576 \
  --config /absolute/path/to/drampool.yaml
```

Replace the IP addresses, RDMA NIC name, capacity and block size with your deployment values. Allow traffic between the configured control and transport endpoints and the HIXL communication port.

| Parameter | Description |
| --- | --- |
| `--addr` | DramPool control address, reachable by every DramStore client. |
| `--nics` | RDMA NIC names used for pinned-memory registration. |
| `--pool-size-gb` | Host memory contributed by this process, in GiB. Leave memory for the OS and runtime buffers. |
| `--kvcache-block-sizes` | Supported KV block sizes in **bytes**, matching the blocks sent by DramStore; this is not vLLM's token `block_size`. Multiple sizes are space-separated. |
| `--kvcache-block-proportions` | Optional relative capacity shares, one per block size; equal shares by default. |
| `--ttl-minutes` | Optional absolute block lifetime, default `120` minutes. |
| `--config` | Runtime YAML path; default `examples/drampool.yaml`, relative to the working directory. |

Wait for `DramPool service ready` in the logs. With `health.port` enabled, check readiness with:

```bash
curl --fail http://192.168.1.10:9100/health
```

Configure the inference-side `UcmPipelineStore` with `store_pipeline: "Dram"`. Its `node_control_endpoints` and `node_transport_manager_ids` must match the pool addresses, and its local endpoints must appear in the pool's routing configuration. Start inference only after the pool is ready. Send `SIGINT` or `SIGTERM` to stop DramPool gracefully.

### Metrics (optional)

DramStore operation metrics are implemented on `develop`; there is no separate DramStore metrics switch. DramPool snapshot export requires [PR #1394](https://github.com/ModelEngine-Group/unified-cache-management/pull/1394) or a version containing that implementation.

**DramPool:** add the following top-level section to the runtime YAML passed through `--config`:

```yaml
metrics:
  enabled: true
  output_dir: /var/log/ucm/drampool
  interval_ms: 10000
```

The daemon atomically replaces `drampool_metrics.json` in `output_dir` every 10 seconds. Snapshot collection and export are disabled when `metrics.enabled` is omitted or `false`; the PR's example YAML explicitly enables them. An empty `output_dir` uses `logger.dir`, and `interval_ms` must be positive when enabled.

**DramStore:** merge the following into the inference-side `ucm_connector_config` for the `Dram` pipeline:

```yaml
ucm_connectors:
  - ucm_connector_name: "UcmPipelineStore"
    ucm_connector_config:
      store_pipeline: "Dram"
      # Keep the other DramStore connection settings.
      drampool_resource_metrics_enable: true
      drampool_resource_log_path: /var/log/ucm/drampool/drampool_metrics.json
      drampool_resource_metrics_interval_sec: 15
```

These three options control the client-side reader of DramPool snapshots, not DramStore's own operation metrics. Setting a nonempty path enables the reader by default; set `drampool_resource_metrics_enable: false` to disable it. The read interval defaults to 15 seconds, and the reader runs only in the scheduler process (`device_id < 0`). The snapshot must be visible and readable from that process; mount the output directory into it if the pool and inference run in different containers or hosts. The path refers to the inference process's filesystem.

Send an inference request, then verify DramStore and imported DramPool metrics through vLLM:

```bash
curl http://<vllm-ip>:<vllm-port>/metrics | grep -E '^ucm:(dramstore_|drampool_)'
```

DramPool exports a file snapshot rather than an HTTP `/metrics` endpoint; `health.port` serves only `/health`. Use [the Dram dashboard](https://github.com/ModelEngine-Group/unified-cache-management/blob/develop/examples/metrics/grafana_dram.json) to inspect both components. Metrics refresh through vLLM's connector-statistics collection during inference; see [UCM Metrics Observability](../metrics/metrics.md) for Prometheus and Grafana setup.

## Performance

### Overview

The following TTFT (time to first token) results are transcribed from `TTFT-性能测试结果.xlsx`, `Sheet1`. Average output length is 1,000 tokens; average input length is shown in the table. The workbook labels the three scenarios `Baseline`, `hit - 50` and `hit - 90`. Only rows with complete TTFT results are included.

TTFT speedup is calculated as **(Baseline TTFT / Scenario TTFT − 1) × 100%**, using the same convention as [PipelineStore](pipeline_store.md). Percentages are rounded to one decimal place; they express the gain in inverse TTFT, rather than the percentage reduction in latency.

| **Input length** | **Requests** | **Concurrent** | **Baseline TTFT** | **hit - 50 TTFT** | **TTFT Speedup (%)** | **hit - 90 TTFT** | **TTFT Speedup (%)** |
| ---------------: | -----------: | -------------: | ----------------: | ----------------: | -------------------: | ----------------: | -------------------: |
|            4 000 |            4 |              1 |             893.8 |             702.5 |           **+27.2%** |             639.2 |           **+39.8%** |
|            4 000 |           16 |              4 |            1092.5 |            1020.0 |            **+7.1%** |             922.1 |           **+18.5%** |
|            4 000 |           64 |             16 |            1789.4 |            1731.8 |            **+3.3%** |            1697.7 |            **+5.4%** |
|            4 000 |          128 |             32 |            2491.3 |            2076.5 |           **+20.0%** |            2151.3 |           **+15.8%** |
|            8 000 |            4 |              1 |            1716.2 |            1111.7 |           **+54.4%** |             671.7 |          **+155.5%** |
|            8 000 |           16 |              4 |            2105.9 |            1707.8 |           **+23.3%** |             986.0 |          **+113.6%** |
|            8 000 |           64 |             16 |            3919.6 |            2774.0 |           **+41.3%** |            1768.6 |          **+121.6%** |
|            8 000 |          128 |             32 |            5939.1 |            3243.9 |           **+83.1%** |            2383.2 |          **+149.2%** |
|           16 000 |            4 |              1 |            3949.0 |            2321.5 |           **+70.1%** |             801.9 |          **+392.5%** |
|           16 000 |           16 |              4 |            5422.5 |            3318.2 |           **+63.4%** |            1242.9 |          **+336.3%** |
|           16 000 |           64 |             16 |            8579.4 |            4977.5 |           **+72.4%** |            2264.1 |          **+278.9%** |
|           16 000 |          128 |             32 |           65085.5 |           29686.6 |          **+119.2%** |           18672.3 |          **+248.6%** |
|           32 000 |            4 |              1 |            9781.7 |            6070.6 |           **+61.1%** |            2055.6 |          **+375.9%** |
|           32 000 |           16 |              4 |           12574.2 |            8200.6 |           **+53.3%** |            2686.5 |          **+368.1%** |
|           32 000 |           64 |             16 |           46070.9 |           37792.1 |           **+21.9%** |           22434.8 |          **+105.4%** |
|           32 000 |          128 |             32 |          178751.1 |          149170.5 |           **+19.8%** |           96544.9 |           **+85.1%** |

Across these runs, TTFT speedup is **3.3%–119.2%** for `hit - 50` and **5.4%–392.5%** for `hit - 90`. At input length 4,000 and concurrency 32, `hit - 90` has higher TTFT than `hit - 50`.

The workbook does not specify the TTFT unit, model, device count, software versions, baseline configuration or hit-ratio methodology. TTFT values retain the source unit; the speedup calculation is independent of that unit. Results describe these test runs only.
