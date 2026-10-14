# DramPool

[English](drampool.md) | [中文](drampool_zh.md)

DramPool 将主机 DRAM 组成 KV 缓存池，供多个推理实例复用。DramStore 是 UCM 的客户端，通过 HIXL 读写池中的 KV 块。缓存块有有效期，重启 DramPool 会清空内存缓存。

## 环境要求

- 昇腾 A3 及以上硬件，CANN **9.1.0 及以上**，以及兼容的驱动和 HIXL 库。
- Linux 环境，DramPool 与推理节点之间具备 RDMA 网络，主机可用内存足以容纳配置的缓存池和运行时缓冲区。
- 已安装 UCM 和兼容的 vLLM-Ascend。推理侧集成步骤参见 [Quickstart-vLLM-Ascend](../../getting-started/quickstart_vllm_ascend.md)。

## 启动 DramPool

### 编译

在 UCM 仓库根目录加载 CANN 环境，并按 A3 平台编译：

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export PLATFORM=ascend-a3
pip install -v -e . --no-build-isolation
```

若 HIXL 单独安装，编译前将 `HIXL_ROOT` 设置为其安装目录，启动前将其库目录加入 `LD_LIBRARY_PATH`。确认编译日志包含 `P2P HIXL transport backend is enabled`。后续昇腾型号应使用对应硬件的平台设置。

### 配置与启动

复制 [examples/drampool.yaml](../../../../examples/drampool.yaml) 作为运行配置。将 `transport.device_ids` 设置为可用设备 ID，并在 `transport.endpoints` 中配置 DramPool 和所有 DramStore 客户端的控制地址（`two_sided`）及传输地址（`one_sided`）。DramPool 的 `two_sided` 地址必须与 `--addr` 一致。

例如，DramPool 对应的配置为：

```yaml
transport:
  device_ids: [0]
  hixl:
    listen_port: 26666
    enable_cs: false
  endpoints:
    - two_sided: "192.168.1.10:9000"
      one_sided: "192.168.1.10:4501"
    # 在此补充 DramStore 客户端地址。
health:
  port: 9100
```

将上述设置合并到复制的配置文件，在仓库根目录启动程序（editable 编译会将程序安装到 `bin/`）：

```bash
./bin/drampool \
  --addr 192.168.1.10:9000 \
  --nics mlx5_0 \
  --pool-size-gb 64 \
  --kvcache-block-sizes 1048576 \
  --config /absolute/path/to/drampool.yaml
```

按实际部署替换 IP、RDMA 网卡名、容量和块大小，并确保控制地址、传输地址及 HIXL 通信端口之间的网络连通。

| 参数 | 说明 |
| --- | --- |
| `--addr` | DramPool 控制地址，所有 DramStore 客户端均需能够访问。 |
| `--nics` | 用于锁页内存注册的 RDMA 网卡名。 |
| `--pool-size-gb` | 当前进程贡献的主机内存容量，单位为 GiB；需为操作系统和运行时缓冲区预留内存。 |
| `--kvcache-block-sizes` | 支持的 KV 块大小，单位为**字节**，须与 DramStore 发送的块匹配；与 vLLM 按 token 计的 `block_size` 含义不同。多个值以空格分隔。 |
| `--kvcache-block-proportions` | 可选，各块大小对应的容量权重；默认等分，每种块大小需对应一个权重。 |
| `--ttl-minutes` | 可选，缓存块的绝对有效期，默认 `120` 分钟。 |
| `--config` | 运行配置路径；默认 `examples/drampool.yaml`，相对于当前工作目录。 |

日志出现 `DramPool service ready` 表示服务已就绪。启用 `health.port` 后可检查：

```bash
curl --fail http://192.168.1.10:9100/health
```

推理侧使用 `UcmPipelineStore`，设置 `store_pipeline: "Dram"`。`node_control_endpoints` 和 `node_transport_manager_ids` 须与池端地址一致，客户端本地地址也须加入池端路由配置。先启动 DramPool，确认就绪后再启动推理；向 DramPool 发送 `SIGINT` 或 `SIGTERM` 可正常退出。

### Metrics（可选）

在推理侧 UCM YAML 中配置以下**顶层**参数，放在 `ucm_connector_config` 外：

```yaml
enable_metrics: true
# 可选的指标启用清单；省略时使用内置指标。
metrics_config_path: /absolute/path/to/examples/metrics/metrics_configs.yaml
```

UCM metrics 默认开启，设置 `enable_metrics: false` 可关闭。自定义指标文件须对推理进程可读。发送推理请求后检查：

```bash
curl http://<vllm-ip>:<vllm-port>/metrics | grep '^ucm:'
```

这些参数通过 vLLM 导出 UCM connector 指标。当前版本尚未导出 DramPool 服务端指标，`health.port` 仅提供 `/health`。可使用 Prometheus 和 Grafana，采集间隔至少为 5 秒；配置参见 [UCM Metrics Observability](../metrics/metrics.md)。vLLM 在推理过程中收集 connector 统计时，UCM 指标才会更新。

## 性能

### 测试说明

以下 TTFT（首 token 延迟）结果来自 `TTFT-性能测试结果.xlsx` 的 `Sheet1`。平均输出长度为 1,000 tokens，平均输入长度见表格。原表的三个场景为“基线”、“hit - 50”和“hit - 90”，仅列出 TTFT 数据完整的测试行。

TTFT 加速百分比按 **（基线 TTFT / 对应场景 TTFT − 1）× 100%** 计算，与 [PipelineStore](pipeline_store.md) 的口径一致，保留一位小数。该值表示 TTFT 倒数的提升，不是延迟下降百分比。

| **输入长度** | **请求数** | **最大并发数** | **基线 TTFT** | **hit - 50 TTFT** | **TTFT 加速 (%)** | **hit - 90 TTFT** | **TTFT 加速 (%)** |
| -----------: | ---------: | -------------: | ------------: | ----------------: | ----------------: | ----------------: | ----------------: |
|        4 000 |          4 |              1 |         893.8 |             702.5 |        **+27.2%** |             639.2 |        **+39.8%** |
|        4 000 |         16 |              4 |        1092.5 |            1020.0 |         **+7.1%** |             922.1 |        **+18.5%** |
|        4 000 |         64 |             16 |        1789.4 |            1731.8 |         **+3.3%** |            1697.7 |         **+5.4%** |
|        4 000 |        128 |             32 |        2491.3 |            2076.5 |        **+20.0%** |            2151.3 |        **+15.8%** |
|        8 000 |          4 |              1 |        1716.2 |            1111.7 |        **+54.4%** |             671.7 |       **+155.5%** |

在这些测试中，`hit - 50` 的 TTFT 加速为 **3.3%–54.4%**，`hit - 90` 为 **5.4%–155.5%**。输入长度为 4,000、并发数为 32 时，`hit - 90` 的 TTFT 高于 `hit - 50`。

原表未注明 TTFT 单位、模型、设备数量、软件版本、基线配置和命中率测试方法。TTFT 数值保留原表单位，加速百分比计算不依赖该单位。结果仅代表这些测试场景。
