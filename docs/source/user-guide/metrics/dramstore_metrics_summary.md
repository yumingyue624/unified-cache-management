# DramStore metrics inventory and latency diagnosis

本文基于 `feat/dramstore-metrics` 分支当前代码，回答三个问题：现有埋点有哪些，
LOOKUP/DUMP/LOAD 如何横向对比和归并，以及请求总时延升高时如何快速定位阶段。
当前配置共包含 37 个 DramStore/DramPool Counter、5 个 Gauge 和 13 个 Histogram。

> 结论先行：三种操作的 Task/Request 指标结构已经基本对称，适合在展示层按
> `operation` 归并；当前指标名不带动态 label，因此这里建议用 recording rule 或
> Grafana 表格归并，不建议仅为展示重命名已有时序。Histogram 适合判断一批请求的
> 分布和趋势，不能还原任意一条具体请求。若要从慢点直接下钻到那一条请求，需补充
> trace/exemplar，而不是给 histogram 增加高基数 `request_id` label。

## 1. 指标层级和基数

一次 DramStore 调用是一个 **Task**。Task 按目标节点拆成一个或多个 **Request**，
这些 Request 可能并行；每个 DramPool Request 又可包含多个 entry。三层计数不能相加。

| 层级 | 含义 | 数量关系 | 当前主要指标 |
| --- | --- | --- | --- |
| Task | 一次 LOOKUP/DUMP/LOAD API 调用 | 1 次调用 = 1 Task | `dramstore_<op>_tasks_*`, `dramstore_<op>_task_duration_seconds` |
| Request | Task 按 DramPool 节点拆出的子请求 | 1 Task = 0..N Request | `dramstore_<op>_requests_*`, `dramstore_<op>_request_duration_seconds` |
| Server request | DramPool 实际处理的请求 | 通常对应成功提交的 client Request；异常窗口不保证严格相等 | `drampool_<op>_requests_total`, `drampool_<op>_duration_seconds` |
| Entry/byte | Request 内的数据项和物理传输量 | 1 Request = 1..N entry | 当前仅有 DramPool DUMP/LOAD bytes；没有 DramStore entry 计数 |

## 2. LOOKUP、DUMP、LOAD 对比矩阵

下表中的 `<op>` 可替换为 `lookup`、`dump`、`load`。三类操作均使用相同的
Histogram buckets：100 us 至 30 s；经 vLLM connector 暴露时统一换算成 seconds。

| 观察面 | 统一指标模式 | LOOKUP | DUMP | LOAD | 是否可归并 | 说明 |
| --- | --- | :---: | :---: | :---: | --- | --- |
| Task 接收成功 | `dramstore_<op>_tasks_submitted_total` | ✓ | ✓ | ✓ | 是，按 operation 行展示 | 仅入队成功才增加 |
| Task 入队拒绝 | `dramstore_<op>_tasks_rejected_total` | ✓ | ✓ | ✓ | 是 | TaskManager 不接收或 submission queue 满；不属于 submitted |
| Task 成功 | `dramstore_<op>_tasks_succeeded_total` | ✓ | ✓ | ✓ | 是 | accepted Task 的最终状态 |
| Task 失败 | `dramstore_<op>_tasks_failed_total` | ✓ | ✓ | ✓ | 是 | 包含 timeout；分析失败原因时不要再与 timeout 相加 |
| Task 超时 | `dramstore_<op>_task_timeouts_total` | ✓ | ✓ | ✓ | 是 | failed 的子集 |
| Task 总时延 | `dramstore_<op>_task_duration_seconds` | ✓ | ✓ | ✓ | 是 | 从 Submit 开始到最终结算；包含 Task 排队和所有子 Request 完成 |
| Task 排队 | `dramstore_<op>_task_queue_duration_seconds` | ✓ | ✓ | ✓ | 是 | 从 Submit 开始到 TaskManager worker 取出 submission |
| Request 完成 | `dramstore_<op>_requests_completed_total` | ✓ | ✓ | ✓ | 是 | 成功和失败均计数 |
| Request 失败 | `dramstore_<op>_requests_failed_total` | ✓ | ✓ | ✓ | 是 | completed 的子集 |
| Request 提交错误 | `dramstore_<op>_request_submit_errors_total` | ✓ | ✓ | ✓ | 是 | TaskManager 到 NodeActor 的同步提交失败；不等同远端执行失败 |
| Request 总时延 | `dramstore_<op>_request_duration_seconds` | ✓ | ✓ | ✓ | 是 | NodeActor 接收 Request 到完成；包含节点 pending、连接/限流等待、客户端传输及远端处理 |
| DramPool 请求数 | `drampool_<op>_requests_total` | ✓ | ✓ | ✓ | 是 | 由本地 DramPool 累计快照导入，带 endpoint/source 维度 |
| DramPool 服务时延 | `drampool_<op>_duration_seconds` | ✓ | ✓ | ✓ | 是 | 服务端视角；与 client Request 时延存在包含关系，不能相加 |
| 前置事件等待 | `dramstore_dump_prerequisite_duration_seconds` | — | ✓ | — | 否，DUMP 专属 | 在 Task Submit 之前等待 compute event，故不包含在 DUMP Task duration 内 |
| 前置事件错误 | `dramstore_dump_prerequisite_errors_total` | — | ✓ | — | 否，DUMP 专属 | prerequisite 等待失败 |
| 确认物理字节 | `drampool_<op>_bytes_total` | — | ✓ | ✓ | 可在 DUMP/LOAD 间对比 | LOOKUP 无数据面 bytes；来自服务端快照 |

### 公共连接、恢复和资源指标

| 指标 | 类型 | 含义 | 推荐展示/告警 |
| --- | --- | --- | --- |
| `dramstore_connect_attempts_total` | Counter | 连接尝试 | `rate()`，与 failure 同图 |
| `dramstore_connect_failures_total` | Counter | 连接失败 | failure / attempts 比率和绝对速率 |
| `dramstore_fence_attempts_total` | Counter | 超时恢复 fence 尝试 | 与 deadline recovery 对齐观察 |
| `dramstore_fence_failures_total` | Counter | fence 提交或完成失败 | 非零速率告警 |
| `dramstore_deadline_recoveries_total` | Counter | Request timeout 触发节点恢复 | 非零通常可解释批量长尾 |
| `dramstore_stale_replies_total` | Counter | 旧 epoch/已退休请求的迟到回复 | 非零提示超时、恢复或网络长尾 |
| `drampool_capacity_bytes` | Gauge | DramPool 总容量 | Stat + time series；跨 exporter 不求和 |
| `drampool_used_bytes` | Gauge | 已用容量 | 与 capacity 计算使用率 |
| `drampool_available_bytes` | Gauge | 可用容量 | 校验 `used + available ≈ capacity` |
| `drampool_resource_snapshot_timestamp_seconds` | Gauge | 最新服务端快照时间 | `time() - metric` 作为 freshness |
| `drampool_resource_reporter_leader` | Gauge | 本进程是否 reporter leader | 每 host/source 应只有一个新鲜 leader |
| `drampool_resource_read_errors_total` | Counter | 快照读取/解析错误 | `rate()` 非零告警 |

## 3. 一条请求的时延由什么组成

### LOOKUP / LOAD

近似关键路径为：

```text
Task total
  = TaskManager queue
  + normalize / split / dispatch overhead
  + max(Request branch 1, ..., Request branch N)
  + completion aggregation overhead

Request branch
  = NodeActor pending (断连、重连或 inflight 限流)
  + reply-slot / encode / local transport submit
  + network request
  + DramPool service
  + network reply / flag observation
  + fence recovery（发生超时时）
```

Request 分支可能并行，因此 Task 总时延应与最慢 Request 分支比较，不能与所有
Request 时延求和。`drampool_<op>_duration_seconds` 位于 client Request 的内部，
也不能再加到 `dramstore_<op>_request_duration_seconds` 上。

### DUMP

DUMP API 的调用方感知总时延还多一个 Task 外阶段：

```text
DUMP API wall time
  ≈ dramstore_dump_prerequisite_duration_seconds
  + dramstore_dump_task_duration_seconds
```

这里两个 histogram 的同分位数仍然**不能直接相加**；上式只表达单次调用的计时边界。
当前 DUMP Task histogram 从 prerequisite 成功后提交开始计时。

### 当前可以快速判断什么

| 现象 | 对照指标 | 优先结论 |
| --- | --- | --- |
| Task p99 高，queue p99 同时高 | Task total vs Task queue | 本地 TaskManager 排队/容量压力 |
| Task p99 高，queue 低，Request p99 高 | Task total vs Request total | 慢点进入节点请求路径 |
| Client Request 与 DramPool service 同时高 | Request total vs server duration | 服务端处理或数据面是主要嫌疑 |
| Client Request 高，DramPool service 低 | 两者差距增大 | NodeActor pending、连接恢复、client transport 或网络盲区 |
| DUMP API 慢但 Task 不慢，prerequisite 高 | prerequisite vs Task total | compute event gating，不是 DramPool 慢 |
| deadline recovery / fence / stale reply 激增 | 恢复 counters + Request heatmap | 超时恢复放大长尾，先看连接和服务端健康 |
| used/capacity 接近 1 且 DUMP 长尾 | resource gauges + DUMP latency | 容量压力相关；需结合服务端拒绝/淘汰指标确认因果 |

## 4. 推荐 Grafana 表格

### 4.1 总览表：每个 operation 一行

| Operation | QPS | Success % | Reject/s | Timeout/s | Task p50 | Task p95 | Task p99 | Queue p99 | Request p99 | Server p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| LOOKUP | `rate(submitted)` | `succeeded / submitted` | `rate(rejected)` | `rate(timeout)` | histogram | histogram | histogram | histogram | histogram | histogram |
| DUMP | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 |
| LOAD | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 |

这张表用于横向比较，不把不同层级的计数混为一个“请求数”。建议同时保留
`worker_rank`、engine/实例和 `drampool_endpoint` 作为 dashboard filters。

### 4.2 慢请求分解表：每个 operation 一行

| Operation | Task p99 | Queue p99 | Request p99 | Server p99 | Client-server gap（诊断值） | 专属阶段 | 判断 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| LOOKUP | total | queue | client request | pool lookup | `Request p99 - Server p99` | — | gap 仅用于趋势定位 |
| DUMP | total | queue | client request | pool dump | 同上 | prerequisite p99 | prerequisite 在 Task 外 |
| LOAD | total | queue | client request | pool load | 同上 | — | 同 LOOKUP |

`p99(total) - p99(stage)` 和 `p99(A) + p99(B)` 都不是严格的单请求分解，因为各
Histogram 的 p99 通常来自不同样本。表中的 gap 只能用作同一过滤条件和窗口下的
趋势诊断，并应明确标注 `diagnostic, not additive`。

### 4.3 推荐面板顺序

| 行 | 面板 | 目的 |
| --- | --- | --- |
| 1 | QPS、成功率、拒绝率、超时率 Stat/Time series | 先判断影响面和错误类型 |
| 2 | 三操作 Task p50/p95/p99 Time series | 看用户可感知长尾及操作差异 |
| 3 | Task / queue / client Request / server p99 同图 | 找时延在哪一层开始抬升 |
| 4 | 四类 duration Heatmap，按 operation 重复 | 看多峰、离群、分布漂移；只画 p99 会丢失这些信息 |
| 5 | connect/fence/recovery/stale counters | 解释网络和恢复型长尾 |
| 6 | capacity/used/available、snapshot freshness | 解释容量和 telemetry 自身健康 |

## 5. PromQL 模板

以下使用 connector 暴露后的 metric 名称；实际部署若附加 namespace，请在名称前补上
该前缀，并保留 `le` 以及需要对比的实例/operation 维度。

```promql
# LOOKUP Task p99
histogram_quantile(
  0.99,
  sum by (le) (
    rate(dramstore_lookup_task_duration_seconds_bucket[$__rate_interval])
  )
)

# LOOKUP Task 平均时延；_sum / _count 必须使用相同过滤条件
sum(rate(dramstore_lookup_task_duration_seconds_sum[$__rate_interval]))
/
sum(rate(dramstore_lookup_task_duration_seconds_count[$__rate_interval]))

# accepted Task 成功率；timeout 已包含于 failed，不要把两者相加
sum(rate(dramstore_lookup_tasks_succeeded_total[$__rate_interval]))
/
sum(rate(dramstore_lookup_tasks_submitted_total[$__rate_interval]))

# DramPool 使用率；按 endpoint/有效 exporter 分别计算，不跨重复 exporter 求和
drampool_used_bytes / drampool_capacity_bytes

# 快照陈旧秒数
time() - drampool_resource_snapshot_timestamp_seconds
```

由于当前指标采用每个 operation 一个 metric family，Grafana 表格可用三个 query 加
`operation=LOOKUP|DUMP|LOAD` 静态字段后 merge。若查询频繁，可建立规范的 recording
rules；不要在采集端给现有时序追加 `request_id`、block key 等无界 label。

## 6. “某一条慢请求”如何下钻

Histogram 只保存 bucket count、count 和 sum，不保存每次 observation 的身份。因此：

| 需求 | 当前 Histogram | 推荐补充 |
| --- | --- | --- |
| 看最近 5 分钟 p99、分布、多峰 | 可以 | Grafana time series + heatmap |
| 比较 LOOKUP/DUMP/LOAD 哪个阶段整体变慢 | 可以，但仅统计相关 | 统一表格和 filters |
| 精确看到某一条 Task 的各阶段耗时 | 不可以 | 分布式 trace/span |
| 从 p99 热点跳到代表性慢请求 | 不可以 | Histogram exemplar 关联 `trace_id`/`span_id` |

推荐为一次 Task 建 root span，节点 Request 建 child span，阶段至少包括
`task.queue`、`node.pending`、`client.prepare_submit`、`network_round_trip`、
`drampool.service`；DUMP 再增加位于 root Task 之前或其父 span 下的
`dump.prerequisite_wait`。只对慢请求/错误请求采样，在 exemplar 中保留 trace 关联，
不要把 request ID 放进 Prometheus label。

如果短期不引入 tracing，最低成本的补强是新增低基数、同构的阶段 Histogram（如
`dramstore_<op>_request_pending_duration_seconds` 和
`dramstore_<op>_client_overhead_duration_seconds`）。它能改善“总体阶段归因”，但仍不能
证明某一个 p99 Task 就对应另一个指标的 p99 Request。

## 7. 当前盲区和建议优先级

| 优先级 | 缺口 | 影响 | 建议 |
| --- | --- | --- | --- |
| P0 | 无单请求关联 | 无法从异常 bucket 下钻到具体慢请求 | trace + exemplar；保留低基数 metrics 做告警 |
| P0 | Request duration 未拆 NodeActor pending/client/network | client 高而 server 低时无法快速归因 | 增加 pending 和 client-side prepare/transport 阶段计时 |
| P1 | DramPool service 内部阶段未在当前分支呈现 | server duration 高时仍需日志/服务端诊断 | 服务端按 LOOKUP/DUMP/LOAD 定义互斥或明确嵌套的阶段 Histogram/span |
| P1 | 缺少 Task/Request entry 数和 client bytes | 难以区分请求变大与实现变慢 | 增加低基数 counter；用 bytes/s 与 duration 联合判断 |
| P2 | 三操作靠名称而非 operation label | Dashboard query 重复 | 先用 recording rule 统一；只有兼容性规划后再考虑新 family |
| P2 | 100 us–30 s 共用 buckets 较宽 | 快操作的低延迟区域分辨率有限 | 用真实分布和 SLO 调整 buckets；变更时保持生产/导入契约一致 |

阶段指标设计时要先声明区间是**互斥**还是**包含**。互斥阶段可以在同一条 trace 上
相加；包含阶段只用于下钻，不能重复计入总时延。并行分支的关键路径取最大值而不是求和。

## 8. 参考原则

- Prometheus metric naming：单一 metric 表达单一单位/数量，Counter 使用 `_total`；
  operation 等维度通常更适合作为有界 label，而不是复制进名字。
- Prometheus histogram：跨实例聚合 bucket 后用 `histogram_quantile()`；不要平均
  预计算 quantile，也不要假设不同阶段的同分位数可相加。
- Grafana：Time series 适合趋势，Heatmap 适合观察 histogram 分布随时间的变化、
  多峰和离群，Table 适合 LOOKUP/DUMP/LOAD 的同口径横向对比。
- OpenTelemetry exemplar：让聚合 metric 的代表性 observation 关联 trace/span，
  适合从延迟热点下钻到具体请求，同时避免 Prometheus 高基数标签。

官方参考：

- [Prometheus metric and label naming](https://prometheus.io/docs/practices/naming/)
- [Prometheus histograms and summaries](https://prometheus.io/docs/practices/histograms/)
- [Prometheus recording rules](https://prometheus.io/docs/practices/rules/)
- [Grafana heatmap](https://grafana.com/docs/grafana/latest/visualizations/panels-visualizations/visualizations/heatmap/)
- [Grafana Prometheus query examples](https://grafana.com/docs/grafana/latest/datasources/prometheus/query-editor/)
- [OpenTelemetry metrics data model and exemplars](https://opentelemetry.io/docs/specs/otel/metrics/data-model/)
