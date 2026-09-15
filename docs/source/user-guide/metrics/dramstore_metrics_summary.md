# DramStore metrics inventory and latency diagnosis

本文基于 `feat/dramstore-metrics` 分支当前代码，回答三个问题：现有埋点有哪些，
LOOKUP/DUMP/LOAD 如何横向对比和归并，以及请求总时延升高时如何快速定位阶段。
本文只讨论 DramStore 客户端侧指标，不包含 DramPool 服务端内部指标。

> 结论先行：三种操作的 Task/Request 指标结构已经基本对称，适合在展示层按
> `operation` 归并；当前指标名不带动态 label，因此这里建议用 recording rule 或
> Grafana 表格归并，不建议仅为展示重命名已有时序。Histogram 适合判断一批请求的
> 分布和趋势，不能还原任意一条具体请求。若要从慢点直接下钻到那一条请求，需补充
> trace/exemplar，而不是给 histogram 增加高基数 `request_id` label。

## 1. 指标层级和基数

一次 DramStore 调用是一个 **Task**。Task 按目标节点拆成一个或多个 **Request**，
这些 Request 可能并行；Task 和 Request 两层计数不能相加。

| 层级 | 含义 | 数量关系 | 当前主要指标 |
| --- | --- | --- | --- |
| Task | 一次 LOOKUP/DUMP/LOAD API 调用 | 1 次调用 = 1 Task | `dramstore_<op>_tasks_*`, `dramstore_<op>_duration_ms` |
| Request | Task 按目标节点拆出的子请求 | 1 Task = 0..N Request | `dramstore_<op>_requests_*`, `dramstore_<op>_request_duration_ms` |

## 2. LOOKUP、DUMP、LOAD 对比矩阵

下表中的 `<op>` 可替换为 `lookup`、`dump`、`load`。以下均使用代码中的原始指标名，
指标按 Counter（累计次数）、Gauge（当前值）、Histogram（时延分布）分类。

### 2.1 Counter：累计次数

用于观察吞吐、拒绝、失败和超时；通常通过 `rate()` 查看每秒速率。

| 观察面 | 统一指标模式 | LOOKUP | DUMP | LOAD | 是否可归并 | 说明 |
| --- | --- | :---: | :---: | :---: | --- | --- |
| Task 接收成功 | `dramstore_<op>_tasks_submitted_total` | ✓ | ✓ | ✓ | 是，按 operation 行展示 | 仅入队成功才增加 |
| Task 入队拒绝 | `dramstore_<op>_tasks_rejected_total` | ✓ | ✓ | ✓ | 是 | TaskManager 不接收或 submission queue 满；不属于 submitted |
| Task 成功 | `dramstore_<op>_tasks_succeeded_total` | ✓ | ✓ | ✓ | 是 | accepted Task 的最终状态 |
| Task 失败 | `dramstore_<op>_tasks_failed_total` | ✓ | ✓ | ✓ | 是 | 包含 timeout；分析失败原因时不要再与 timeout 相加 |
| Task 超时 | `dramstore_<op>_task_timeouts_total` | ✓ | ✓ | ✓ | 是 | failed 的子集 |
| Request 完成 | `dramstore_<op>_requests_completed_total` | ✓ | ✓ | ✓ | 是 | 成功和失败均计数 |
| Request 失败 | `dramstore_<op>_requests_failed_total` | ✓ | ✓ | ✓ | 是 | completed 的子集 |
| Request 超时 | `dramstore_<op>_request_timeouts_total` | ✓ | ✓ | ✓ | 是 | Request 最终以 Timeout 结算时计一次，是 requests_failed 的子集；包含到期和节点恢复中被置为 Timeout 的请求，不与 failed 相加 |

#### 公共连接和恢复 Counter

这些指标由三个操作共用，不按 LOOKUP/DUMP/LOAD 分开计数。

| 指标 | 含义 | 推荐展示/告警 |
| --- | --- | --- |
| `dramstore_connect_attempts_total` | 连接尝试 | `rate()`，与 failure 同图 |
| `dramstore_connect_failures_total` | 连接失败 | failure / attempts 比率和绝对速率 |
| `dramstore_fence_attempts_total` | 超时恢复 fence 尝试 | 与 deadline recovery 对齐观察 |
| `dramstore_fence_failures_total` | fence 提交或完成失败 | 非零速率告警 |
| `dramstore_deadline_recoveries_total` | Request timeout 触发节点恢复 | 非零通常可解释批量长尾 |
| `dramstore_stale_replies_total` | 旧 epoch/已退休请求的迟到回复 | 非零提示超时、恢复或网络长尾 |

### 2.2 Gauge：当前值

以下资源由 LOOKUP、DUMP、LOAD 共用，统一展示，不按操作重复计数。
沿用 Posix 的使用量/容量命名方式，占用率在展示端计算：`used / capacity` 或
`size / capacity`（容量大于零时）。Gauge 直接展示当前采样值，不使用 `rate()`。

| 资源 | 当前使用量指标 | 容量指标 | 单位和含义 |
| --- | --- | --- | --- |
| Task 提交队列 | `dramstore_task_queue_size` | `dramstore_task_queue_capacity` | Task 数；等待 TaskManager worker 取出，不包含正在处理的 Task |
| Request 完成队列 | `dramstore_completion_queue_size` | `dramstore_completion_queue_capacity` | RequestCompleted 事件数；等待 TaskManager 聚合 |
| NodeScheduler 请求队列 | `dramstore_scheduler_request_queue_size` | 无固定容量 | 所有 runner 的待取 Request 总数；不包含已取出的 batch 和 NodeActor pending |
| NodeScheduler 事件队列 | `dramstore_scheduler_event_queue_size` | 无固定容量 | 所有 runner 的待取 NodeEvent 总数；不包含已取出的 batch |
| Transport 普通命令队列 | `dramstore_transport_queue_size` | `dramstore_transport_queue_capacity` | 所有 worker 合计的 Transmit/Connect 入队配额占用；在出队后归还配额时减少，不包含实际传输 |
| Transport 恢复命令队列 | `dramstore_transport_fence_queue_size` | `dramstore_transport_fence_queue_capacity` | Fence 命令独立保留的入队配额，避免普通命令挤占恢复容量 |
| Reply buffer | `dramstore_reply_buffer_used_bytes` | `dramstore_reply_buffer_capacity_bytes` | 按 slot 对齐后的 stride 计算租用字节数和预分配总字节数；reply 已到但尚未释放的 slot 仍算占用；不是 reply 有效载荷大小，释放 slot 不归还预分配内存 |
| 活跃 Task 的 entry 配额 | `dramstore_io_entries_used` | `dramstore_io_entries_capacity` | 已接纳处理的 Task 占用的 entry 数；不包含 submission queue 中的 Task |

另有 `dramstore_tasks_active`：已进入 activeTasks、尚未完成结算的 Task 数；不包含
排队 Task，也不包含已完成但调用方尚未领取的结果。

每组指标由固定工作线程读取现有状态，目标每秒采样一次，避免多个线程的 Gauge
缓存互相覆盖。空闲时继续采样；工作线程长时间执行操作时，采样会延后，停止后不再
刷新。多 runner 的队列长度是依次采样后的合计，不是同一时刻的全局原子快照。
这些指标用于观察持续压力，短暂队满仍应结合 `tasks_rejected_total` 等 Counter。
当前尚未单独暴露 NodeActor pending/inflight 数。

### 2.3 Histogram：时延分布

以下指标单位均为毫秒，用于查看平均值、p50/p95/p99 和分布。
Task 总时延 buckets 为 0.1–5000 ms；可能快速结束的 Task 排队、Request 和 DUMP
前置等待从 0.01 ms 开始，分别覆盖至 500 ms、5000 ms 和 500 ms。

| 观察面 | 统一指标模式 | LOOKUP | DUMP | LOAD | 是否可归并 | 说明 |
| --- | --- | :---: | :---: | :---: | --- | --- |
| 端到端总时延 | `dramstore_<op>_duration_ms` | ✓ | ✓ | ✓ | 是 | 从 Submit 开始到最终结算；包含 Task 排队和所有子 Request 完成 |
| Task 排队 | `dramstore_<op>_task_queue_duration_ms` | ✓ | ✓ | ✓ | 是 | 从 Submit 开始到 TaskManager worker 取出 submission |
| Task 到 Request | `dramstore_<op>_task_to_request_duration_ms` | ✓ | ✓ | ✓ | 是 | 从 Task 成功入队到所有 Request 完成构造；包含 Task 排队 |
| Request 总时延 | `dramstore_<op>_request_duration_ms` | ✓ | ✓ | ✓ | 是 | Request 构造完成到 QueueCompletion；包含调度排队、节点 pending、发送、远端处理及回复 |
| Request 调度排队 | `dramstore_<op>_request_queue_duration_ms` | ✓ | ✓ | ✓ | 是 | Request 构造完成到 NodeActor 接收，主要是 NodeScheduler queue |
| Request pending | `dramstore_<op>_request_pending_duration_ms` | ✓ | ✓ | ✓ | 是 | NodeActor 接收到 StartRequest；包含断连、重连及 inflight 限流等待 |
| Request 准备 | `dramstore_<op>_request_prepare_duration_ms` | ✓ | ✓ | ✓ | 是 | reply slot 获取及请求编码 |
| Transport 排队 | `dramstore_<op>_request_transport_queue_duration_ms` | ✓ | ✓ | ✓ | 是 | submitTransport 成功到 TransportExecutor worker 取出 |
| Transport 发送 | `dramstore_<op>_request_transport_send_duration_ms` | ✓ | ✓ | ✓ | 是 | TransportExecutor 调用 backend Transmit 到 TCP Send 返回 |
| Request 远端 | `dramstore_<op>_request_remote_duration_ms` | ✓ | ✓ | ✓ | 是 | TCP 发送完成到 ReplyObserved；主要是远端执行及回复等待 |
| 前置事件等待 | `dramstore_dump_prerequisite_duration_ms` | — | ✓ | — | 否，DUMP 专属 | 在 Task Submit 之前等待 compute event，故不包含在 DUMP Task duration 内 |

## 3. 一条请求的时延由什么组成

### LOOKUP / LOAD

近似关键路径为：

```text
Task total
  = Task to Request（TaskManager queue + normalize / route / split）
  + max(Request branch 1, ..., Request branch N)
  + completion aggregation overhead

Request branch
  = NodeScheduler queue
  + NodeActor pending (断连、重连或 inflight 限流)
  + reply-slot / encode
  + TransportExecutor queue
  + TCP send
  + remote service / reply observation
  + fence recovery（发生超时时）
```

Request 分支可能并行，因此 Task 总时延应与最慢 Request 分支比较，不能与所有
Request 时延求和。当前阶段 Histogram 可以定位整体热点，但不同 Histogram 的同分位数
不能直接相加或相减来还原某一条具体请求。

### DUMP

DUMP API 的调用方感知总时延还多一个 Task 外阶段：

```text
DUMP API wall time
  ≈ dramstore_dump_prerequisite_duration_ms
  + dramstore_dump_duration_ms
```

这里两个 histogram 的同分位数仍然**不能直接相加**；上式只表达单次调用的计时边界。
当前 DUMP 端到端 Histogram 从 prerequisite 成功后提交开始计时。

### 当前可以快速判断什么

| 现象 | 对照指标 | 优先结论 |
| --- | --- | --- |
| Task p99 高，queue p99 同时高 | Task total vs Task queue | 本地 TaskManager 排队/容量压力 |
| Task p99 高，queue 低，Request p99 高 | Task total vs Request total | 慢点进入节点请求路径 |
| DUMP API 慢但 Task 不慢，prerequisite 高 | prerequisite vs Task total | compute event gating，不是远端服务慢 |
| deadline recovery / fence / stale reply 激增 | 恢复 counters + Request heatmap | 超时恢复放大长尾，先看连接和服务端健康 |
| Task queue 接近容量 | task_queue_size / task_queue_capacity + rejected | TaskManager 持续积压 |
| Transport queue 高，send 时延也高 | transport_queue_size / capacity + transport_send | 传输 worker 消费跟不上 |
| Reply buffer 或 entry 配额接近容量 | reply_buffer_used_bytes / reply_buffer_capacity_bytes、io_entries_used / io_entries_capacity | 请求长期占用资源；结合 remote 时延、超时和恢复指标排查 |
| 完成队列持续增长 | completion_queue_size + tasks_active | TaskManager 完成聚合跟不上 |

## 4. 推荐 Grafana 表格

### 4.1 总览表：每个 operation 一行

| Operation | QPS | Success % | Reject/s | Timeout/s | Task p50 | Task p95 | Task p99 | Task→Request p99 | Queue p99 | Request p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| LOOKUP | `rate(submitted)` | `succeeded / submitted` | `rate(rejected)` | `rate(timeout)` | histogram | histogram | histogram | histogram | histogram | histogram |
| DUMP | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 |
| LOAD | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 | 同上 |

这张表用于横向比较，不把不同层级的计数混为一个“请求数”。建议同时保留
`worker_rank` 和 engine/实例作为 dashboard filters。

### 4.2 慢请求分解表：每个 operation 一行

| Operation | Task p99 | Task→Request p99 | Queue p99 | Request p99 | 专属阶段 | 判断 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| LOOKUP | total | task pre-request | queue | client request | — | Task→Request 高时先看 queue；否则定位路由和拆分 |
| DUMP | total | task pre-request | queue | client request | prerequisite p99 | prerequisite 在 Task 外 |
| LOAD | total | task pre-request | queue | client request | — | 同 LOOKUP |

`p99(total) - p99(stage)` 和 `p99(A) + p99(B)` 都不是严格的单请求分解，因为各
Histogram 的 p99 通常来自不同样本。

### 4.3 推荐面板顺序

| 行 | 面板 | 目的 |
| --- | --- | --- |
| 1 | QPS、成功率、拒绝率、超时率 Stat/Time series | 先判断影响面和错误类型 |
| 2 | 三操作 Task p50/p95/p99 Time series | 看用户可感知长尾及操作差异 |
| 3 | Task / Task→Request / queue / Request p99 同图 | 找时延在哪一层开始抬升 |
| 4 | Task / Task→Request / queue / Request duration Heatmap，按 operation 重复 | 看多峰、离群、分布漂移；只画 p99 会丢失这些信息 |
| 5 | connect/fence/recovery/stale counters | 解释网络和恢复型长尾 |

## 5. PromQL 模板

以下使用代码中的原始 metric 名称；实际部署若附加 namespace，请在名称前补上该前缀，
并保留 `le` 以及需要对比的实例/operation 维度。时延查询结果的单位为毫秒。

```promql
# LOOKUP Task p99
histogram_quantile(
  0.99,
  sum by (le) (
    rate(dramstore_lookup_duration_ms_bucket[$__rate_interval])
  )
)

# LOOKUP Task 平均时延；_sum / _count 必须使用相同过滤条件
sum(rate(dramstore_lookup_duration_ms_sum[$__rate_interval]))
/
sum(rate(dramstore_lookup_duration_ms_count[$__rate_interval]))

# accepted Task 成功率；timeout 已包含于 failed，不要把两者相加
sum(rate(dramstore_lookup_tasks_succeeded_total[$__rate_interval]))
/
sum(rate(dramstore_lookup_tasks_submitted_total[$__rate_interval]))

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
`task.queue`、`node.pending`、`client.prepare_submit`、`network_round_trip`；
DUMP 再增加位于 root Task 之前或其父 span 下的
`dump.prerequisite_wait`。只对慢请求/错误请求采样，在 exemplar 中保留 trace 关联，
不要把 request ID 放进 Prometheus label。

当前低基数阶段 Histogram 已覆盖 request queue、pending、prepare、transport queue、
transmit 和 remote，能够改善
总体阶段归因，但仍不能证明某一个 p99 Task 就对应另一个指标的 p99 Request。

## 7. 当前盲区和建议优先级

| 优先级 | 缺口 | 影响 | 建议 |
| --- | --- | --- | --- |
| P0 | 无单请求关联 | 无法从异常 bucket 下钻到具体慢请求 | trace + exemplar；保留低基数 metrics 做告警 |
| P1 | 缺少 Task/Request entry 数和 client bytes | 难以区分请求变大与实现变慢 | 增加低基数 counter；用 bytes/s 与 duration 联合判断 |
| P2 | 三操作靠名称而非 operation label | Dashboard query 重复 | 先用 recording rule 统一；只有兼容性规划后再考虑新 family |
| P2 | 当前 buckets 沿用 UCM 同类模块的通用范围 | 真实分布可能与通用范围不完全匹配 | 上线后结合真实分布和 SLO 继续调整 buckets |

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
