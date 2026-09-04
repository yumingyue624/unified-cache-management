# DramPool Metrics 实现方案

## 1. 背景

DramPool 的整体部署架构由两个部分组成：

  1. DramStore 以动态链接库（SO）的形式集成于 vLLM 进程中，可直接复用 UCM 现有的 metrics 系统，仅需补充相应的指标定义与采集逻辑。

  2. 每台主机独立部署一个 DramPool 进程。由于 DramPool 与 vLLM 运行在不同的进程中，其内部采集的指标无法直接接入 vLLM 的
     metrics 系统。因此，采用与 YuanRong 一致的跨进程指标收集机制：DramPool 周期性地将指标快照写入本地文件，UCM 后台的 Reporter 线程读取最新快照，并将指标更新至 Scheduler 进程内的 ucmmetrics，最终通过现有的 vllm_connector 出口统一暴露。


## 2. 总体架构

> **整体设计**：跨进程链路参考 YuanRong；DramPool 进程内采集选择 UCM 已有的 **thread-local + 双 buffer** 实现，不重新实现 YuanRong 的全局 `MetricSlot` 体系。

![DramPool Metrics 跨进程采集与统一回流](./drampool_metrics_architecture_v2.svg)

可编辑源文件：[DramPool Metrics 跨进程架构图](./drampool_metrics_architecture_v2.excalidraw)。

```text
DramPool 业务线程
    │  Counter / Gauge / Histogram
    ▼
thread-local MetricBuffer（双 buffer）
    │  每 10 秒切换和汇总
    ▼
DramPool MetricsReporter
    │  追加一条全量 JSON 快照
    ▼
drampool_metrics.log
    │  Leader Scheduler 从文件尾部读取
    ▼
DramPoolResourceReporter
    │  累计值转增量
    ▼
ucmmetrics → MetricsDispatcher → vllm_connector → Prometheus
```

方案分成两段：

| 范围 | 组件 | 职责 |
| --- | --- | --- |
| DramPool 进程内 | `DramPoolMetrics` | 指标注册、业务线程打点、thread-local 双 buffer 管理 |
| DramPool 进程内 | `DramPoolMetricsReporter` | 每 10 秒汇总、维护累计值、写全量 JSON 快照 |
| Scheduler 进程内 | `DramPoolResourceReporter` | 抢占 host leader、读取最新快照、计算增量、回流 UCM |
| UCM 现有链路 | `ucmmetrics` / `MetricsDispatcher` | 汇总并通过 `vllm_connector` 输出 |


## 3. 进程内采集方案选型

候选方案包括 YuanRong 的固定 `MetricSlot` 和 UCM 的 thread-local 双 buffer。

![DramPool 进程内采集方案对比](./drampool_metrics_buffer_comparison_v2.svg)

可编辑源文件：[进程内采集方案对比图](./drampool_metrics_buffer_comparison_v2.excalidraw)。

### 3.1 YuanRong MetricSlot 实现

YuanRong 在进程内预分配固定大小的 slot 数组：

```cpp
constexpr size_t MAX_METRIC_NUM = 1024;
std::array<MetricSlot, MAX_METRIC_NUM> g_slots;
```

每个指标由固定 `id` 定位一个 `MetricSlot`。Slot 按 cache line 对齐，保存指标描述和运行值：

```cpp
struct alignas(64) MetricSlot {
    uint16_t id;
    MetricType type;
    std::string name;
    std::string suffix;

    std::atomic<uint64_t> u64Value;   // Counter 或 Histogram count
    std::atomic<int64_t> i64Value;    // Gauge
    std::atomic<uint64_t> sum;        // Histogram sum
    std::atomic<uint64_t> max;
    std::atomic<uint64_t> periodMax;
    HistBuckets histBuckets;
    std::mutex histMutex;
    bool used;
};
```

其工作方式如下：

| 类型 | 更新方式 | 汇总方式 |
| --- | --- | --- |
| Counter | `fetch_add()` 更新 `u64Value` | 读取累计值，与 `g_last` 做差得到周期增量 |
| Gauge | `fetch_add()` 或 `fetch_sub()` | 读取当前值，与上一次值比较 |
| Histogram | 持有 `histMutex` 后更新 count、sum、max 和 bucket | 持有同一把锁复制 bucket，并计算 p50/p90/p99 |

YuanRong MetricSlot 的加锁逻辑如下：

- `g_stateMutex` 用于防止并发生成快照，以及快照与初始化、重置同时执行。在当前单快照线程且只初始化一次的生产模型下，它主要是防御性保护。
- Counter/Gauge 使用原子变量，业务打点不加锁，也不受 `g_stateMutex` 影响。
- Histogram 使用每个 Slot 独立的 `histMutex`，保证 count、sum、max 和 buckets 的一致性。
- 快照读取 Histogram 时会短暂阻塞同一指标的写入，不同 Histogram 之间互不影响。

`Counter`、`Gauge` 和 `Histogram` 对外只是持有 `MetricSlot*` 的轻量句柄，业务调用不再执行名称查找。`Tick()` 到达监控周期后调用 `BuildSummary()`，扫描已注册 slot，用 `g_last` 保存的上一轮值计算 delta，最后输出 JSON Lines。

YuanRong 的 `ScopedTimer` 同样是一个轻量 RAII 对象：构造时保存 `steady_clock::now()`，析构时将微秒耗时写入对应 Histogram slot。

这种方案结构紧凑，固定 ID 的访问成本低。不过所有线程会直接更新同一组 slot；尤其 Histogram 更新需要竞争 slot 内的互斥锁。若用于 DramPool，还需要重新引入 slot 注册、全局数组、周期快照和输出等整套逻辑。

### 3.2 UCM thread-local + 双 buffer 实现

UCM 已经提供完整的 `MetricBuffer`、线程注册、读写切换和多线程聚合实现。每个打点线程拥有自己的 `thread_local MetricBuffer`，不同业务线程不会更新同一个 map。

每个 `MetricBuffer` 包含两个 `InnerBuffer`：

```cpp
struct MetricBuffer {
    struct InnerBuffer {
        std::unordered_map<MetricId, double> counterStats;
        std::unordered_map<MetricId, double> gaugeStats;
        std::unordered_map<MetricId, HistogramStat> histogramStats;
    };

    InnerBuffer innerBufs[2];
    std::atomic<int> writeIdx{0};
    std::atomic<int> activeWriteIdx{-1};
};
```

两层隔离降低了热路径竞争：

1. **线程之间隔离**：TaskWorker、CompletionPoller、Receiver 和 GC 线程分别写自己的 thread-local buffer。
2. **采集与写入隔离**：同一线程的两个 InnerBuffer 轮换使用，业务线程写新 buffer 时，Reporter 汇总旧 buffer。


## 4. 双 Buffer 详细实现

### 4.1 线程首次注册

每个业务线程首次调用 `UpdateStats()` 时注册自己的 buffer：

```text
UpdateStats(metric, value)
    │
    ├─ 当前线程是否已注册？── 是 ───────────────┐
    │                                         │
    └─ 否：将 thread-local buffer 加入 buffers │
                                              ▼
                                      写入当前 write buffer
```

注册列表由 `DramPoolMetrics` 管理。注册是低频操作，只在一个线程第一次打点时发生；后续打点不再获取注册锁。DramPool 的工作线程数量固定，因此已注册 buffer 可以保留到 Server 停止。

### 4.2 单次写入协议

单次写入通过 `WriteGuard` 完成：

```cpp
int BeginWrite()
{
    while (true) {
        int idx = writeIdx.load(std::memory_order_acquire);
        activeWriteIdx.store(idx, std::memory_order_release);

        if (writeIdx.load(std::memory_order_acquire) == idx) {
            return idx;
        }

        activeWriteIdx.store(NO_ACTIVE_WRITER, std::memory_order_release);
    }
}
```

这里的二次检查用于处理下面的竞争窗口：业务线程第一次读到 `writeIdx=0` 后，Reporter 可能立即把它切换成 1。业务线程设置 `activeWriteIdx=0` 后再次读取 `writeIdx`；如果已经变化，则撤销本次占用并重新选择新 buffer，避免继续写入 Reporter 即将读取的旧 buffer。

获得写 buffer 后，根据指标类型执行：

```cpp
switch (metric.type) {
    case COUNTER:
        buffer.counterStats[id] += value;
        break;
    case GAUGE:
        buffer.gaugeStats[id] = value;
        break;
    case HISTOGRAM:
        ++buffer.histogramStats[id].bucketCounts[bucketIndex];
        buffer.histogramStats[id].sum += value;
        break;
}
```

写入完成时，`WriteGuard` 析构并将 `activeWriteIdx` 恢复为 `NO_ACTIVE_WRITER`。

### 4.3 Reporter 切换协议

每 10 秒，`DramPoolMetricsReporter` 对每个已注册 buffer 执行：

```text
业务线程                               Reporter
   │                                     │
   │  持续写 Buffer[0]                    │
   │                                     │ exchange writeIdx: 0 → 1
   │                                     │
   │  下一次打点开始写 Buffer[1]          │ wait activeWriteIdx != 0
   │                                     │
   │  持续写 Buffer[1]                    │ 读取并汇总 Buffer[0]
   │                                     │ clear Buffer[0]
   ▼                                     ▼
```

对应步骤为：

1. `SwitchBuffer()` 使用原子 exchange 切换 `writeIdx`，并返回旧索引。
2. `WaitNoActiveWriter(oldIdx)` 等待切换瞬间已经进入旧 buffer 的单次写入结束。
3. Reporter 读取旧 buffer，将其合并到本轮汇总结果。
4. 清空旧 buffer，为下下轮写入复用。

等待只覆盖切换瞬间的一次短写操作，不等待整个业务线程，也不阻塞业务线程继续写新 buffer。

### 4.4 多线程汇总规则

Reporter 遍历所有已注册线程 buffer，并按类型合并：

| 类型 | 单线程 buffer 语义 | 跨线程合并规则 |
| --- | --- | --- |
| Counter | 本周期增量 | 所有线程求和 |
| Gauge | 当前线程本周期最后值 | 资源 Gauge 优先由 Reporter 直接采样 owner 状态 |
| Histogram | 本周期 bucket count 与 sum | bucket 逐项求和，sum 求和 |

内存使用量、队列深度、entry 数量等资源 Gauge 不依赖“哪个线程最后写入”，由 Reporter 在生成快照时直接读取 `BufferManager`、`MetadataManager` 和队列的当前状态。这样 Gauge 的语义稳定，也避免跨线程最后写入顺序不确定。

### 4.5 周期数据转全量快照

双 buffer 汇总得到的是最近 10 秒的增量。`DramPoolMetricsReporter` 内部维护一个累计快照：

```text
cumulative_counter += interval_counter
cumulative_histogram.bucket[i] += interval_histogram.bucket[i]
cumulative_histogram.sum += interval_histogram.sum
current_gauge = sample_current_value()
```

写文件时输出累计 Counter、累计 Histogram 和当前 Gauge。因此每一行都是独立的终点状态，UCM Reporter 无需读取或拼接历史行。

## 5. 指标打点方式

### 5.1 Counter 与 Gauge

建议第一阶段覆盖以下核心指标：

| 类型 | 示例 | 打点位置 |
| --- | --- | --- |
| 请求 Counter | `drampool_dump_requests_total`、`drampool_load_requests_total`、`drampool_lookup_requests_total` | `TaskWorker::ProcessOneRequest()` |
| Item Counter | dump/load item、lookup hit/miss | 各操作的 item 处理循环或最终结果结算点 |
| 字节 Counter | dump/load bytes | 成功提交或最终完成点，需统一语义 |
| 失败 Counter | allocation、transport、timeout、response failure | 对应失败分支 |
| 容量 Gauge | capacity、used、available bytes | Reporter 周期采样 `BufferManager` |
| 队列 Gauge | request/completion queue depth | Reporter 周期采样队列 |
| 元数据 Gauge | entry count | Reporter 周期采样 `MetadataManager` |

同一个指标只能选择一个明确结算点。例如 `dump_bytes_total` 如果定义为“成功写入 DramPool 的字节数”，就应在异步传输成功并完成 `StoreEnd()` 后累加，而不是在请求刚进入 TaskWorker 时累加。

### 5.2 ScopedTimer

同步阶段可以直接使用 RAII：

```cpp
Status MetadataManager::StoreBegin(...)
{
    ScopedTimer timer(metrics_, MetricId::STORE_BEGIN_DURATION);
    // 原有 StoreBegin 逻辑
}
```

`ScopedTimer` 使用单调时钟，不受系统时间调整影响：

```cpp
ScopedTimer::~ScopedTimer()
{
    const auto elapsed = steady_clock::now() - start_;
    metrics_.Observe(id_, DurationToMicroseconds(elapsed));
}
```

### 5.3 异步请求耗时

DUMP 和 LOAD 的数据传输跨越 TaskWorker 与 CompletionPoller，不能在 `ProcessDump()` 或 `ProcessLoad()` 内放一个覆盖全过程的栈上 Timer。实现方式是：

1. 请求进入 TaskWorker 时记录 `request_start_us`。
2. 将开始时间随 `CompletionRecord` 传给 CompletionPoller。
3. 响应传输到达 terminal 状态时计算端到端耗时。
4. 只记录一次对应 Histogram。

同步的 metadata、protocol encode 等子阶段仍可以使用 `ScopedTimer`。

## 6. 快照文件实现

### 6.1 文件位置与周期

Metrics 文件使用已有 `g_config.logDir`，采集周期固定为 10 秒。

```text
${g_config.logDir}/drampool_metrics.log
```

### 6.2 JSON Lines 格式

```json
{
  "event": "drampool_metrics_snapshot",
  "version": "v0",
  "time": "2026-09-03T12:00:00+08:00",
  "endpoint": "127.0.0.1:12345",
  "sequence": 42,
  "counters": {
    "drampool_dump_requests_total": 1200
  },
  "gauges": {
    "drampool_used_bytes": 68719476736
  },
  "histograms": {
    "drampool_load_duration_us": {
      "bucket_counts": [12, 36, 81],
      "sum": 92450
    }
  }
}
```

写入规则：

- Reporter 先在内存中构造完整 JSON 和结尾换行符。
- 只有该线程写 metrics 文件，避免多写者交错。
- 以 append 模式写入，不覆盖已有记录。
- 进程崩溃可能留下最后一个半行，读取端负责忽略。
- `sequence` 每成功生成一次快照递增，用于识别重复读取。

### 6.3 生命周期

`DramPoolServer::Init()` 创建 metrics 账本、快照累计器和文件 writer，但不启动线程。`Start()` 在内部消费者准备好后启动 metrics reporter，并且仍然保证 transport service 和 TCP listener 最后启动。

`Stop()` 先停止 metrics reporter，再销毁它引用的 BufferManager、MetadataManager 和队列。所有可变运行对象由 `DramPoolServer` 持有，`DramPoolRuntime` 只保存非 owning 引用。

## 7. Scheduler Reporter 实现

### 7.1 启动入口

Scheduler 初始化 DramStore 时调用：

```python
start_drampool_resource_reporter(config)
```

Reporter 只在 Scheduler/EngineCore 角色启动；device worker 不启动该线程。

### 7.2 Leader 选举

同一 host 上多个 Scheduler 连接的是同一个 DramPool，必须保证每个快照只回流一次。

```python
identity = sha256(f"{endpoint}|{Path(log_path).resolve()}".encode()).hexdigest()[:24]
lock_path = shared_dir / f"ucm_drampool_metrics_{identity}.lock"
state_path = shared_dir / f"ucm_drampool_metrics_{identity}.json"
```

Reporter 使用 `flock(LOCK_EX | LOCK_NB)` 抢锁：

- 抢锁成功后一直持有打开的文件 fd。
- Leader 正常退出时主动 unlock 并关闭 fd。
- Leader 崩溃时，内核随 fd 关闭自动释放锁。
- 不需要租约或心跳。
- Reporter 启动时只尝试抢锁一次；抢锁失败后关闭本次打开的文件 fd，并直接退出 Reporter 线程。

DramPool Reporter 与当前 YuanRong Reporter 保持一致，不提供运行期间的 Reporter 故障接任能力。Scheduler 进程退出时，内核释放文件锁仅用于完成资源回收，不触发其他已退出 Reporter 重新选主。

### 7.3 从尾部读取最新完整行

每轮采集执行：

```text
open(log_path, "rb")
    → seek(EOF)
    → 从尾部向前读取 64 KiB
    → 如果末尾没有换行，丢弃最后一个半行
    → 从后向前选择最新完整 JSON
```

单条快照通常小于 64 KiB，所以正常情况一次读取即可命中。即使文件包含大量历史快照，读取成本也与整份文件大小无关。

### 7.4 累计值转换为增量

DramPool 写入累计值，UCM 账本接收本轮增量：

```python
delta = current - previous if current >= previous else current
```

`current < previous` 表示 DramPool 重启或指标 reset，本轮直接上报 `current`。相同规则应用于 Histogram bucket count 和 sum。Gauge 直接使用最新值。

### 7.5 State 文件

Leader 用 state 文件记录最后一次成功回流的累计值：

```text
ucm_drampool_metrics_<hash>.json
```

写入过程为：

```text
写 .<pid>.tmp
    → 关闭完整临时文件
    → os.replace(tmp_path, state_path)
```

`os.replace()` 对 `state_path` 是原子整体替换，因此读取者看到的只会是旧版本或新版本，不会读到截断 JSON。

处理顺序选择“先回流、后更新 state”。这样不会因为提前更新 state 而永久丢失指标；如果进程恰好在两步之间崩溃，后续重新启动的 Scheduler Reporter 可能重复回流上一轮数据。该链路不引入事务协调，语义为偏向不丢数据的 at-least-once。

## 8. 回流 UCM 统一出口

Counter 和 Gauge 合并成一个 updates 字典：

```python
updates = counter_deltas | gauges
ucmmetrics.update_stats(updates)
```

调用后，数据进入 Scheduler/EngineCore 进程内的 C++ metrics 单例，并落到 Reporter 线程自己的 thread-local `MetricBuffer`。此后它们与 `posix_`、`cache_` 和 connector hit 指标完全相同，由现有 `MetricsDispatcher` 分发到 `vllm_connector`。

Histogram 快照包含 bucket delta 和 sum delta，而当前 Python `update_stats(dict[str, float])` 只能表达单个 observation。为了保持 Histogram 分布，实施时需要为现有 metrics binding 增加一个批量写入 Histogram bucket 的生产接口；Counter/Gauge 仍使用上述 `update_stats(updates)`。不采用按 bucket 重放大量伪造 observation 的方式。

## 9. 核心异常处理

Metrics 是旁路能力。文件写入、解析或回流失败时记录告警并等待下一轮，不能阻塞或终止 DUMP、LOAD、LOOKUP 主流程。

| 异常 | 处理方式 |
| --- | --- |
| metrics 文件不存在或为空 | 本轮记录 warning，等待下一轮 |
| 文件末尾为半行 | 忽略半行，读取上一条完整记录 |
| JSON 解析失败 | 本轮不更新 state，等待下一轮 |
| state 文件不存在 | 将当前累计值作为 baseline，避免首次启动回灌全部历史值 |
| state 文件损坏 | 记录 warning，按无 state 处理 |
| Leader Scheduler 退出 | fd 随进程关闭并释放文件锁；当前推理实例不再回流节点指标 |
| DramPool Counter reset | `delta=current`，不产生负数 |
| 文件写入失败 | 记录错误，DramPool 业务继续运行 |
