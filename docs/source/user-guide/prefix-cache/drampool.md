# DramPool

[English](drampool.md) | [中文](drampool_zh.md)

DramPool pools host DRAM for KV cache reuse across inference instances. DramStore is the UCM client that reads and writes KV blocks in the pool through HIXL. Cached blocks have a finite lifetime; restarting DramPool clears its in-memory cache.

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

In the inference-side UCM YAML, set the following **top-level** options, outside `ucm_connector_config`:

```yaml
enable_metrics: true
# Optional metric enable-list; omit to use the built-in metrics.
metrics_config_path: /absolute/path/to/examples/metrics/metrics_configs.yaml
```

UCM metrics are enabled by default; set `enable_metrics: false` to disable them. A custom metrics file must be readable by the inference process. Send an inference request, then check:

```bash
curl http://<vllm-ip>:<vllm-port>/metrics | grep '^ucm:'
```

These options expose UCM connector metrics through vLLM. This version does not export DramPool server metrics, and `health.port` exposes only `/health`. Use Prometheus and Grafana with a scrape interval of at least 5 seconds; see [UCM Metrics Observability](../metrics/metrics.md) for configuration. UCM metrics refresh when vLLM collects connector statistics during inference.

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

Across these runs, TTFT speedup is **3.3%–54.4%** for `hit - 50` and **5.4%–155.5%** for `hit - 90`. At input length 4,000 and concurrency 32, `hit - 90` has higher TTFT than `hit - 50`.

The workbook does not specify the TTFT unit, model, device count, software versions, baseline configuration or hit-ratio methodology. TTFT values retain the source unit; the speedup calculation is independent of that unit. Results describe these test runs only.
