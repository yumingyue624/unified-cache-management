# DramPool metrics：vLLM / UCM / DramStore 实现设计

设计基线：`upstream/develop`，提交 `f3dd330ed7353f7552aaa1b45a85c5e1f7d4e153`（2026-09-05）。2026-09-08 已 fetch 并以该提交的源码核对。本文只做设计，不包含实现代码。

范围修订：DramPool 进程内部全部由同事负责。本文不设计其打点、双 buffer、Reporter、写文件线程、内存统计或 Server 生命周期；只定义消费端所需的对接契约。仓库根 `drampool.md` 仍是 DramPool 设计权威，不新增任何 DramPool 启动参数。

## 1. 结论与实施边界

保留两条采集链路，汇入同一套 UCM metrics 出口：

```text
DramStore C++ 业务线程
    └─ 本进程 libmetrics → thread-local buffer ─────────────┐
                                                         │
同事提供的本机 DramPool 累计快照文件                         │
    └─ Scheduler 中 DramPoolResourceReporter               │
         └─ 校验 / 选主 / 差分 / 批量导入 → libmetrics ──────┤
                                                         ▼
                                                 MetricsDispatcher
                                                         ▼
                                                 UCMConnectorStats
                                                         ▼
                                   各进程沿用既有 stats 上报路径
                                                         ▼
                                                  UCMPromMetrics
                                                         ▼
                                            vLLM 现有 HTTP /metrics
```

本侧实施分为四块：

1. DramStore：记录客户端任务、节点请求、结果、耗时和连接恢复事件。
2. DramStore Python 接入层：参考 YuanRong 新增 Reporter 线程，读取本机 DramPool 文件，每轮读取 state、计算累计差值、导入 UCM 后保存 state；启动时仅选主一次，失败线程退出。
3. UCM shared metrics：补充预聚合 Histogram 的生产导入接口。
4. vLLM 集成：各进程复用已有 UCM 指标采集和 vLLM 上报路径，仅补新增 DramPool 指标的节点来源信息和配置定义。

不新增独立 HTTP exporter；不把文件读取放到推理调用栈；不修改 KV 网络协议。DramStore 的指标不能替代 DramPool 的物理内存和实际 RDMA 字节统计。

## 2. upstream/develop 已有能力与缺口

| 位置 | 已有行为 | 本次设计中的动作 |
| --- | --- | --- |
| `ucm/shared/metrics/cc/domain/metrics.*` | 单例账本、CachedMetric、TLS 双 buffer、Counter/Gauge/Histogram、破坏性 drain | 复用；补 Histogram 聚合导入，不引入 DramPool 专用账本 |
| `ucm/shared/metrics/cpy/metrics.py.cc` | 标量/字典 update；Histogram 可以导出 bucket 和 sum | 增加 bucket/sum 导入 binding |
| `ucm/metrics_dispatcher.py` | 一次 drain，再分发给 multiproc、vllm_connector；支持 Histogram 合并 | 直接复用，不修改该模块 |
| `ucm/integration/vllm/metrics.py` | 按 rank 携带指标；Prometheus 端支持预聚合 Histogram | 复用既有聚合与校验，仅补新增 DramPool 节点来源标签的传递和导出 |
| `ucm/integration/vllm/patch/scheduler_metrics_patch.py` | 现有 Scheduler 指标采集适配 | 原样复用，不修改采集条件或增加合并 |
| `ucm/store/yuanrongstore/resource_reporter.py` | 文件尾读、flock、累计差分、state 原子替换 | 仅作参考，不修改；在 `ucm/store/dram/resource_reporter.py` 新增独立实现，支持 Histogram，不抽象公共 Reporter |
| `ucm/store/pipeline/connector.py::_dram_pipeline_builder` | 只 Stack Dram | 增加 metrics 预加载和 Reporter 注册入口 |
| `ucm/store/dram/cc/task_manager.*` | Task admission、路由拆分、请求结果聚合、promise 完成 | Task 级计数和耗时的主要落点 |
| `ucm/store/dram/cc/node_actor.*` | 节点请求状态机、回复、超时、fence、连接恢复 | Request 级计数、耗时、entry 结果的主要落点 |

关键事实：现有 vLLM Histogram 出口已能接收 bucket 快照；缺的是 Python → C++ 的导入能力。另一个实际缺口是 Scheduler patch 的条件分支，不能仅完成 Reporter 就认为链路贯通。

修改范围遵循“新功能必需”原则：已有模块满足需求时直接复用，不附带通用加固、重构或格式整理。shared metrics 仅增加预聚合 Histogram 导入入口，复用既有 TLS、WriteGuard 和 drain 路径；不修改既有注册表、双 buffer 内存序或 drain 锁策略。外部快照及导入参数校验放在新增 Reporter 和新增接口内。

## 3. 与 DramPool 同事对齐的最小接口

下面是拟议契约，需要作为双方对接文档确认；它不是要求同事采用某种内部实现。若对方已有确定 schema，消费端通过固定映射适配，保留以下语义即可。

| 字段/约定 | 消费端需要的含义 |
| --- | --- |
| event、schema_version | 能识别 DramPool metrics 记录，拒绝不支持的主版本 |
| source_id / endpoint | 稳定的本机 DramPool 身份；端口应为 DramPool 服务端 endpoint，不是 DramStore 自身 endpoint |
| timestamp | 快照采集时间，带时区或 epoch seconds |
| counters | 本次进程 从启动以来的累计值 |
| gauges | 本次采样的当前值，缺失与零不同 |
| histograms | 固定边界、非累计区间桶计数、总和、单位；均为 进程累计值 |
| 完整性 | UTF-8 JSON Lines；换行标记一条记录完整；每条是可独立消费的全量快照 |
| 文件生命周期 | 稳定的活动文件路径，允许 rename 轮转；同一进程 轮转不能重置累计值 |

Histogram 的 `bucket_counts` 是各区间的计数，而非 Prometheus 文本格式中的累计 `le` 桶。最后一个桶覆盖超过最大有限边界的样本；JSON 不能使用非标准数值 Infinity，可约定固定 schema 隐含最终无穷桶。count 可由所有区间桶求和，若显式携带 count 必须与之相等。

新 schema 建议使用 `v1`，避免两份旧设计中不同 event/结构都叫 `v0`。指标名称可以在 wire 上按组组织，UCM 使用白名单映射到稳定的指标名，不直接注册文件中出现的任意名字。

必须明确两个部署条件：所有候选 Scheduler 能读同一个本机快照；它们的选主 lock/state 目录也映射到同一宿主机目录。不同容器各自的 `/dev/shm` 不能形成 host 级互斥。只读取本机 DramPool，不遍历 DramStore 路由表去采集远端节点。

## 4. 指标模型与命名

使用三个前缀：`dramstore_` 表示客户端事实；`drampool_` 表示同事提供的服务端事实；`drampool_resource_` 表示消费链路自身健康状态。

这些是内部指标名前缀；Prometheus 导出沿用 UCM 的 `vllm_connector_prefix`，默认统一添加 `ucm:`，例如 `ucm:dramstore_load_tasks_submitted_total`。

当前 UCM 以名字定位指标，首版沿用固定名字，不为 operation/status 引入通用动态 label 引擎。下文 `<op>` 展开为 lookup、dump、load 三组注册定义。不要把 task_id、request_id、BlockId、地址、错误字符串放进标签。

### 4.1 DramStore 首版业务指标

| 指标 | 类型 | 精确定义 |
| --- | --- | --- |
| `dramstore_<op>_tasks_submitted_total` | Counter | TaskManager 成功接受进 submissions 队列的任务数 |
| `dramstore_<op>_tasks_rejected_total` | Counter | TaskManager admission 失败数，不进入 submitted |
| `dramstore_<op>_tasks_succeeded_total` | Counter | 已接受任务以成功状态完成的数量 |
| `dramstore_<op>_tasks_failed_total` | Counter | 已接受任务以失败状态完成，含过期和内部 IO 容量拒绝 |
| `dramstore_<op>_task_timeouts_total` | Counter | failed 中最终状态为 Timeout 的子集 |
| `dramstore_<op>_task_duration_us` | Histogram | 从 admission 起点至 task promise 结果完成前，包含排队、路由、远端等待及必要恢复等待 |
| `dramstore_<op>_task_queue_duration_us` | Histogram | 成功入队至 ProcessSubmission 开始 |
| `dramstore_<op>_requests_completed_total` | Counter | NodeActor 结算并生成唯一 RequestCompleted 的节点请求数 |
| `dramstore_<op>_requests_failed_total` | Counter | 上述请求最终 status 失败数 |
| `dramstore_<op>_request_duration_us` | Histogram | NodeActor 接受请求至 QueueCompletion，包含节点内排队和恢复等待 |
| `dramstore_<op>_request_submit_errors_total` | Counter | TaskManager 向 NodeScheduler 移交失败；不计入 NodeActor completed |
| `dramstore_lookup_hit_entries_total` / `miss_entries_total` | Counter | 客户端接受的有效 LOOKUP 回复中的命中/未命中 entry 数 |
| `dramstore_<dump/load>_acknowledged_entries_total` | Counter | 有效回复确认成功的 IO entry 数 |
| `dramstore_<dump/load>_acknowledged_bytes_total` | Counter | 上述成功 entry 的请求长度之和 |
| `dramstore_<dump/load>_failed_entries_total` | Counter | 有效回复中明确失败的 entry 数 |
| `dramstore_<op>_unconfirmed_entries_total` | Counter | 未得到可接受的最终 entry 结果的数量，例如客户端超时 |
| `dramstore_dump_prerequisite_duration_us` / `prerequisite_errors_total` | Histogram / Counter | DramStore::Dump 中等待前置 event 的耗时与失败，独立于 TaskManager task |

一个 task 可拆成多个 node request；DUMP/LOAD 的一个 shard 又会按 tensorSizes 展开成多个 IO entry。因此 tasks、requests、entries、shards、blocks 不能混用，也不能期待客户端 task 数等于服务端 request 数。

`acknowledged_bytes` 是逻辑确认字节数。重复 DUMP key 可能成功但没有新传输，客户端无法凭现有回复区分，故不得将该指标称为 RDMA 实际吞吐或新增缓存字节。真实数据面吞吐取 DramPool 指标。

请求 status 失败不一定意味着全部 entry 失败。若协议解码保留了有效逐项结果，按结果记录成功/失败；若返回路径已丢弃逐项结果，则记 unconfirmed，不推测局部成功。实施时需核对 ReplyService 的解析和错误传播，保留已有有效信息，不改变网络协议。

### 4.2 客户端恢复和资源诊断

首版增加固定 Counter：connect attempts/failures、fence attempts/failures、deadline-triggered recovery、stale replies。重连尝试在提交新命令时记一次，不能每轮 Advance 都增加；请求超时数与一次恢复影响的请求数分开定义。

Queue depth、active requests、used IO entries 等 Gauge 放在第二批：由各 owner 发布并发安全的当前值，再由单一 sampler 汇总。NodeScheduler 可配置多个 runner，不能让多个 NodeActor 往同名 TLS Gauge 各写局部值，现有聚合只会覆盖。也不能从 Python 跨线程直接读取容器 size。

这些状态读取接口的生产用途是运行时可观测性；不为测试添加构造参数或 fake 注入口。首版没有安全 owner 快照时宁可暂不导出该 Gauge。

### 4.3 导入的 DramPool 指标

消费端准备支持容量/使用量、请求/entry 计数、传输字节、失败、耗时 Histogram；具体清单和口径由双方 schema 固定。每项定义必须包含名称、单位、类型、累计/当前值、桶边界、缺失策略。此处不规定同事的具体打点位置。

### 4.4 消费链路健康指标

至少提供 `drampool_resource_snapshot_timestamp_seconds`、`snapshot_age_seconds`、`snapshot_fresh`、`reporter_leader`，以及 read/parse/import/state-write errors 的 Counter。不再提供依赖实例识别的 source restarts 计数。

重复快照不能重复导入 Counter/Histogram，但仍更新 freshness，并周期重发有效 Gauge。快照陈旧时保留最后一次容量值同时标记 stale，不能把缓存使用量清零。初始没有快照时只输出健康指标，不伪造业务数据。

## 5. DramStore 具体实现落点

### 5.1 轻量打点封装

拟新增 `ucm/store/dram/cc/dram_metrics.h`，集中固定 CachedMetric 和轻量计时/更新函数，底层直接调用现有 `UC::Metrics` API。该 C++ 打点封装不创建 singleton 或后台线程；文件 Reporter 由 DramStore Python 接入层单独启动，详见第 6 节。指标关闭或未注册时快速返回；观测异常不改变业务结果，RAII 析构不向外抛异常。

热路径使用预定义名字和 CachedMetric，不构造 JSON、不执行文件 I/O、不保存请求历史。开始时间跟随现有 Submission、ActiveTask、Request 生命周期；计时使用 steady_clock。只增加本地运行字段，不序列化进 KV 协议。

### 5.2 Task 的唯一结算

- `EnqueueTask`：记录 admission 起点；push 成功记 submitted，失败记 rejected。可在返回前使用保存的局部时间完成计数，避免依赖已被另一线程消费的 Submission。
- `ProcessSubmission`：记录 queue duration；把起点移入 ActiveTask。过期、IO 容量不足等提前完成分支也必须结算 failed 和 duration。
- `CompleteRequest`：只在 remainingRequests 降到零时结算一次 task，再清理 ActiveTask 并完成 promise。
- `Run` 异常路径：对已接受但未完成任务统一记录终态，避免遗漏异常清理分支。
- `Shutdown`：以实际完成/取消语义为准。若当前 shutdown 直接放弃任务，不为 metrics 修改业务排空策略；可单列 abandoned，不伪装成功或已投递失败结果。进程崩溃不保证最后的增量可导出。

`Check` 和 `Wait` 只观察结果，不统计 task completed。否则重复 Check、延迟 Wait、调用方不 Wait 都会扭曲数据。task duration 与 Wait 调用阻塞耗时是不同概念。

### 5.3 Request 与 entry 的唯一结算

以 `NodeActor::QueueCompletion` 为 request 主结算点。当前立即过期、pending 过期及 RetireRequest 均汇入这里，适合统一计数。起点在 Handle(Request) 接受请求时记录并随 Request 保留。

逐项结果在 Request 和 EntryResult 同时可见时结算，使用请求原有 BufferRef.length 累加字节。stale/重复 ReplyObserved 在现有 token/epoch 检查处丢弃，只记 stale reply，不增加成功数。

已暴露的请求超时后可能等待 fence 才安全退休：请求 duration 结束于最终 QueueCompletion；“到达 deadline”是另一个诊断事件，不能提前销毁统计上下文或重复结束请求。

### 5.4 链接和启动顺序

`dramstore` 显式链接已有 `metrics` shared target，并为安装后的库补齐 metrics 目录的 RPATH；Python `_preload_metrics` 与现有 Posix/Cache 使用方式保持一致。确保 SO 与 pybind 使用同一份 libmetrics，不把 metrics.cc 再编译进 dramstore，避免两个单例导致数据不可见。

Python metrics 定义注册须在启动业务打点线程与文件 Reporter 前完成。Connector/Store 的独立使用应保留现有未启用 metrics 时 no-op 行为，不能要求用户为了业务运行必须启用监控。

## 6. DramStore 侧新增 Reporter 线程

拟新增 `ucm/store/dram/resource_reporter.py`，类名为 `DramPoolResourceReporter`，启动入口为 `start_drampool_resource_reporter(config)`。这是 DramStore 接入 UCM 时创建的 Python 后台线程，运行在承载 Scheduler DramStore 的进程内，与 `start_yuanrong_resource_reporter` 的放置方式一致。C++ DramStore SO 继续负责业务打点，不再另起第二个文件读取线程。

“对文件做差值”具体是：只读最新完整记录，解析为累计快照，与共享 state 中上次保存的快照做数值差分；不修改生产端文件，不比较文本差异，也不要求逐行回放。每轮重新读取 state，不维护独立的内存差分基线。

### 6.0 对照 YuanRong 的复用与扩展

| YuanRong 现有实现 | DramStore Reporter 设计 |
| --- | --- |
| `start_yuanrong_resource_reporter(config)`：检查开关、路径和 device_id，再启动线程 | 保留入口模式与 Scheduler 角色限制，同进程仅创建一个 Reporter，先完成 metrics 初始化 |
| `threading.Thread` + `_stop_event.wait(interval)` | 沿用，一个候选 Scheduler 一个线程；本轮完成后可中断等待 |
| `_read_latest_complete_line()` | 沿用尾部读取思路，补半行/分块首行处理、最大读取限制与轮转 |
| `YuanRongResourceSnapshot(counters, gauges, timestamp)` | 扩为含 histograms、source 的 DramPoolResourceSnapshot |
| `counter_deltas(current, previous)` | Counter 保留累计差分；增加 Histogram 整体差分和数值回退 reset |
| `ucmmetrics.update_stats(gauges | counter_deltas)` | 原调用继续用于 Counter/Gauge，Histogram 走新增批量 bucket 导入 |
| `_read_previous_counters()` / `_write_previous_counters()` | state 同时保存 counters 与完整 histograms，每轮读取 state 作为基线，导入后保存 |
| `flock` 和临时文件 + `os.replace` | 沿用一次选主和原子保存；候选抢锁失败退出，state 写失败后可能重复回灌 |

首版单独实现 DramPool Reporter，不修改 YuanRong 的行为，也不急于抽象统一 Reporter 基类。两者文件 schema 和 Histogram 支持有差异，优先保持代码直接可读。

内部数据模型明确为：

- `DramPoolResourceSnapshot`：已校验的 source/schema/timestamp，以及 counters、gauges、histograms。
- `HistogramSnapshot`：unit、有限 upper_bounds、bucket_counts、count、sum。边界从 wire 或固定 schema 取得，不能根据数组长度猜测。
- `DramPoolResourceDelta`：counter_deltas、当前 gauges、histogram_deltas；Histogram delta 仍是 bucket_counts/count/sum，而非原始样本列表。
- `ReporterState`：上一次成功保存到文件的累计快照，以及 state 格式版本；不保存所有历史轮次。

解析和差分使用纯函数，选主、文件和线程由 Reporter 管理，不需要向生产对象增加测试构造器。

### 6.1 配置与生命周期

新增的是 UCM 客户端配置，不是 DramPool 参数：

| 配置 | 用途 |
| --- | --- |
| `drampool_resource_log_path` | 当前容器可见的本机活动快照文件路径；未配置不启动 |
| `drampool_resource_metrics_enable` | 显式开关；可默认跟随路径是否配置 |
| `drampool_resource_shared_dir` | 所有本机候选 Scheduler 共享且可写的 lock/state 目录 |

首版采集轮询固定 10 秒，不增加无必要的调参项。新鲜度阈值默认三个约定生产周期，并允许启动宽限；它属于监控判断，不触发业务断连。

`_dram_pipeline_builder` 在 Stack 成功后登记 Reporter；正式 start 必须等待 metrics 注册完成，若实际初始化顺序相反，由 connector 初始化完成阶段激活。只在 Scheduler 角色启动，采用现有明确角色/device_id 约定，不凭 rank==0 代替 Scheduler 判断。

同进程对同一 source 只创建一个 Reporter。所有对象在 fork 后创建；线程不跨 fork 继承。关闭通过 stop Event 唤醒，在线程退出后释放锁；不能 join 超时后仍释放锁而让旧线程继续导入。优先绑定 connector/store 显式关闭，atexit 仅作兜底。

线程的每轮处理顺序固定为：

```text
线程启动 → 读取 source → 尝试 flock 一次
  → 抢锁失败或启动读取/选主异常：结束线程
  → 抢锁成功：进入采集循环
      → 读取并校验最新完整快照
      → 从 state 文件读取上次累计值
      → Counter / Histogram 做差，Gauge 取当前值
      → merge_histogram_stats，再 update_stats
      → 同目录临时文件原子替换 state
      → 更新健康指标，stop_event.wait(10 秒)
```

重复读取同一累计快照时，差值自然为零，不额外增加 sequence 去重或分阶段导入重试记录。循环内异常记录日志，下一轮重新读取 state。

### 6.2 选主

与 YuanRong 一致，线程启动时仅尝试一次 `flock(LOCK_EX | LOCK_NB)`。抢不到锁就结束线程；Leader 停止后，原有候选不会自动接任，只有后来新启动的 Reporter 才再次尝试选主。

锁 key 仍使用稳定 source identity，所有候选共享同一宿主机 lock/state 目录。Leader 持有同一个 fd 直到线程停止，运行中不删除锁文件。保留 source 与配置 endpoint 校验，不增加运行期重新选主机制。

### 6.3 有界读取与校验

每周期重新 open 活动文件，避免 rename 后一直跟随旧 inode。按 64 KiB 分块从尾部向前读取，设内部最大扫描/单记录上限（建议 1 MiB，并与生产端约定），不能无限回扫历史日志。

丢弃没有结尾换行的尾部半行，以及分块起点截断的首行；从最新完整记录开始校验。畸形行可在扫描上限内回退上一条有效记录，同时保留错误计数和 stale 判断。新文件为空或轮转窗口暂时不存在时保留旧状态，下轮重试。

校验内容：event/version/source、必要字段、有限非负 Counter/耗时、整数桶、桶长度/边界、count 与桶计数之和一致、sum 合法。未知可选字段忽略；关键字段缺失或主版本不支持时不导入。schema 不匹配不能以补零或截断桶的方式继续。

### 6.4 累计差分

每轮从 state 读取上次累计 counters/histograms，与当前快照做差。不要求 instance_id 或 sequence，也不保存退役实例列表。

| 情况 | 处理 |
| --- | --- |
| 无有效 state | Counter/Histogram 增量为零，当前值保存为基线；Gauge 立即导入 |
| Counter 当前值不小于上次值 | current - previous |
| Counter 当前值小于上次值 | 推断重置，以当前值作为增量，与 YuanRong 一致 |
| Histogram 所有桶和 sum 均未下降 | 对区间桶和 sum 做差，count 由桶之和得到 |
| Histogram 任一桶或 sum 下降 | 推断该 Histogram 重置，整体以当前 buckets/count/sum 作为增量 |
| 重复读取同一累计值 | 差值为零，无需独立去重 |
| 有 state，但新增指标无旧值 | 旧值按零处理，当前累计值作为增量 |
| state 损坏 | 记录错误，按无有效 state 处理 |

数值回退只是 reset 推断：如果重启后的累计值已超过旧值，就识别不出重启，可能少计；旧快照回退也可能被当成 reset。接受与 YuanRong 同类的简化限制。Histogram 保留边界、单位及 count/sum 一致性校验。

#### 6.4.1 Histogram 差分示例

设有限边界为 `[100, 500, 1000]` us，则四个区间依次为 `≤100`、`(100,500]`、`(500,1000]`、`>1000`。下面的两次快照来自同一次 DramPool 运行，所有数值均是从 DramPool 启动以来累计：

| 字段 | previous | current | 本轮导入 delta |
| --- | --- | --- | --- |
| bucket_counts | `[10, 20, 5, 1]` | `[12, 23, 6, 1]` | `[2, 3, 1, 0]` |
| count | 36 | 42 | 6 |
| sum_us | 12000 | 13900 | 1900 |

本轮新增 6 个样本，平均耗时 `1900 / 6` us。漏读中间快照不影响正常累计差值。相同的当前快照再读一次，Counter 和 Histogram 的新增量均为零。

三个类型分别处理：Counter 做 `current - previous`；Gauge 直接取 current；Histogram 对每个区间桶、count 和 sum 做差。**不对平均值、p50、p99 或桶边界做差。** 若文件额外提供预计算分位数，首版不将其作为可合并 Histogram 导入，分位数由最终 bucket 分布计算。

边界、顺序和单位在同一 schema 中必须完全一致。每个 Histogram 验证 `count == sum(bucket_counts)`；delta 同样满足 `delta_count == sum(delta_bucket_counts)`。对非负时延，sum 不能是负数/NaN/Infinity；delta_count 为零时 delta_sum 应为零（只容忍数值舍入误差）。不能要求 sum 等于桶边界乘计数，那无法代表真实观测总和。

当任一区间桶或 sum 下降时，推断该 Histogram 重置，导入当前完整 buckets/count/sum；首次无 state 则三者一起建立 baseline，不导入历史。禁止仅对下降的桶重置、其他桶继续相减，这会拼出不属于任何真实观测集合的分布。

Counter 的整数计数与字节累计值、Histogram bucket/count 在 Python 和 state 中尽量保留整数，先做差再在 UCM 标量边界转换为 double，避免长期累计量超过 2^53 后先转浮点丢失小增量。Histogram bucket 导入保留 uint64 计数；浮点 sum 单独处理。

#### 6.4.2 state 增加 Histogram 内容

YuanRong 的 state 仅保存 counters；DramPool 必须保存每个 Histogram 的完整累计值和桶定义。以下是消费端 state 的结构示例，不是对同事文件布局的强制要求：

```json
{
  "state_version": 1,
  "source_id": "10.0.0.1:12345",
  "snapshot_schema_version": "v1",
  "timestamp": 1788825600,
  "counters": {"drampool_load_requests_total": 42},
  "histograms": {
    "drampool_load_duration_us": {
      "unit": "us",
      "upper_bounds": [100, 500, 1000],
      "bucket_counts": [12, 23, 6, 1],
      "count": 42,
      "sum": 13900
    }
  }
}
```

Gauge 不参与差分。上例展示 state 的累计数据；当前实现将完整快照包在 state_version/snapshot 中保存，保留 Gauge 仅为复用快照序列化。每轮持锁读取 state，导入后保存当前快照。

### 6.5 state 与交付可靠性

处理顺序：校验快照 → 读取 state → 计算增量 → 导入 metrics → 同目录临时文件原子替换 state。

与 YuanRong 一致，不保留已导入的内存差分基线。若导入成功而 state 保存失败，下轮仍读旧基线，可能重复累计。Histogram 导入成功但标量导入失败时，也不增加防重试状态，下轮可能重复导入 Histogram。

临时文件原子替换只避免半写文件，不保证导入和保存的事务性；导入后、vLLM/Prometheus 看到数据前进程退出也可能丢失指标。整条链路是 best-effort telemetry，不增加 WAL 或下游确认机制。

## 7. shared metrics 的 Histogram 导入

建议生产 API 名为 `MergeHistogramStats`，Python 为 `merge_histogram_stats`，接收“指标名 → 区间桶增量与 sum 增量”的批次。接口不改变已有 observation API。

Python 输入形状固定为 `dict[str, tuple[list[int], float]]`：tuple 的第一个成员为非累计区间桶增量，第二个成员为该轮 sum 增量。它与当前 `get_all_stats_and_clear` 导出的 Histogram tuple 形状一致；count 不单独写入底层，按桶增量求和得到，以免维护两份不一致的计数。以第 6.4.1 节为例，导入项是 `drampool_load_duration_us → ([2, 3, 1, 0], 1900.0)`。

C++ 先校验批次内指标已注册且为 Histogram、bucket 数与注册定义一致、值合法；再向调用线程的 TLS write buffer 累加。它与单次 observation 使用同一个 WriteGuard/聚合路径，下游无需区分数据来源。

binding 要在转换为 uint64 之前拒绝负数、非整数及超范围 bucket 值；C++ 合并还需检查加法溢出。无效批次在校验阶段整体拒绝，不能无声忽略未知指标后仍通知 Reporter 导入成功。空批次是 no-op。C++ 不承担 wire schema 解析，边界与单位对应关系由 Python 已注册的固定定义校验。

完整 Histogram 流程如下：

```text
文件中的 进程累计 bucket_counts / sum
  → Reporter：与 previous 做差
  → pybind：merge_histogram_stats（区间桶增量、sum 增量）
  → C++：合并到 Reporter 线程的 TLS HistogramStat
  → MetricsDispatcher：drain 后向 consumer 累加区间桶
  → UCMConnectorStats：携带 bucket_counts、sum
  → UCMPromMetrics：增加 Histogram 内部区间桶，sum 从 us 缩放为 seconds
  → Prometheus 文本出口：由 Histogram 实现生成累计 le 桶、_count、_sum
```

Reporter、binding、Dispatcher 都不提前做区间桶的前缀和。示例增量 `[2,3,1,0]` 对应本轮 Prometheus `le` 桶增量 `[2,5,6,6]`，只在最终出口表达；若中途先转换再交给现有 Histogram，将发生二次累计。

不能按桶中点伪造 observation 重放：那会改变 sum 和分布精度，且开销与请求数成正比。批量导入开销应与指标数和桶数成正比。

现有标量 update 对未知指标可能静默忽略，Reporter 启动时必须校验需要导入的定义已经注册。Counter/Gauge 与 Histogram 的多个调用不是持久事务；先完成全部业务校验再修改，异常不推进磁盘 state。若出现部分内存更新，记录导入失败并按 best-effort 边界处理，不能声称可以无损回滚。

指标注册在打点/采集线程前完成。多个 consumer 继续经既有 MetricsDispatcher drain；Reporter 只写，不调用 get_all_stats_and_clear。本次不增加共享层通用并发修正。

## 8. vLLM 出口与配置

### 8.1 复用各进程的现有上报路径

与 YuanRong 接入方式一致，Reporter 将增量写入所在 Scheduler 进程的 UCM metrics；Worker 将客户端打点写入各自进程的 UCM metrics。各进程通过既有 get_all_stats_and_clear、MetricsDispatcher 和 connector stats 路径上报。

本功能不新增跨进程汇合，不改变 vLLM 既有聚合、Scheduler 采集条件或调用频率；scheduler_metrics_patch.py 与 upstream/develop 保持一致。指标刷新时机沿用既有调用链，不额外承诺每轮同时采集两侧。

UCMConnectorStats 直接复用既有 Counter/Gauge/Histogram 聚合语义。Scheduler 使用 scheduler rank，Worker 使用各自 rank，本功能不需要改变同 rank/name 的通用合并规则；仅随指标包传递新增 DramPool 来源信息。

### 8.2 节点身份与 Leader 切换

DramStore 指标沿用 engine/worker_rank 标签。导入的 DramPool 指标额外携带稳定 `drampool_endpoint` 来源，以串行化的 ConnectorStats 元数据传递，在创建对应 Prometheus family 时增加该标签；不将所有 UCM 指标全局扩标签。

当前 libmetrics 本身无动态 label，因此首版限制一个进程导入一个本机 DramPool source；Reporter/connector 持有其固定 source 元数据。该限制符合本任务的一机一 DramPool 模型，不设计多源任意汇总。

host Leader 切换可能把节点数据迁移到另一个 vLLM HTTP target。旧 target 的 Gauge 仍可能存在，所以容量/使用量图不能跨 target 求和，应按 endpoint 选择最新且 fresh 的样本；Counter 速率对各序列先 rate 再按 endpoint 汇总，接受交接窗口的误差。不能承诺跨 exporter 迁移保持一条连续 Counter。

只启用 vllm_connector 作为这套节点指标的默认出口，避免同时采集 multiproc 和 connector 后重复相加。Reporter 在没有可用 consumer 时不启动并给出明确诊断，不能读取后静默丢弃。

### 8.3 定义、桶和单位

以 `examples/metrics/metrics_configs.yaml` 为配置源，按项目现有方式同步生成 `ucm/default_metrics_config.py` 并更新指标文档，不能只手改生成文件。

DramStore 时延内部统一微秒，Prometheus 暴露 seconds，沿用 value_scale=1e-6，同时缩放 bucket 边界。Task/Request 时延初始建议有限桶为 100、500、1000、5000、10000、50000、100000、500000、1000000、5000000、10000000、30000000 us，另含最终无穷桶；后续根据压测分布调整，变更需有版本意识。

DramPool Histogram 桶以双方契约为准，消费端必须逐一匹配；不能简单复用一套 DramStore 桶。现有 UCMPromMetrics 通过 Histogram 内部字段导入聚合值，此依赖需要在支持的 prometheus_client/vLLM 组合上验证。

## 9. 文件级实施清单

| 文件/目录 | 计划改动 |
| --- | --- |
| `ucm/store/dram/cc/dram_metrics.h`（新增） | 固定指标句柄、打点/计时轻量封装 |
| `ucm/store/dram/cc/dram_store.cc` | prerequisite 指标、必要入口错误统计 |
| `ucm/store/dram/cc/task_manager.h/.cc` | Task 时间上下文与统一终态打点 |
| `ucm/store/dram/cc/types.h`、`node_actor.h/.cc` | Request 本地时间上下文、entry 结果、恢复事件 |
| `ucm/store/dram/cc/reply_service.cc` | 仅在需要保留有效逐项结果时做最小适配；不改协议 |
| `ucm/store/dram/CMakeLists.txt` | dramstore 链接 libmetrics 和安装 RPATH；不改 drampool target |
| `ucm/store/dram/resource_reporter.py`（新增） | 文件消费、差分、选主、state、健康指标 |
| `ucm/store/pipeline/connector.py` | Dram metrics 预加载、Reporter 登记 |
| `ucm/shared/metrics/cc/api/*`、`cc/domain/*`、`cpy/metrics.py.cc` | Histogram 批量导入及校验 |
| `ucm/metrics_dispatcher.py` | 无代码改动，直接复用已有 drain 和 Histogram 分发能力 |
| `ucm/integration/vllm/ucm_connector.py` | Reporter 激活/关闭、source 元数据传递 |
| `ucm/integration/vllm/metrics.py` | 仅新增 DramPool 节点标签传递和导出；不改既有聚合语义及校验 |
| `ucm/integration/vllm/patch/scheduler_metrics_patch.py` | 无代码改动，复用已有采集路径 |
| metrics YAML、生成默认配置、metrics 文档 | 新增指标定义、单位、桶、查询口径 |

不修改 `ucm/store/dram/cc/drampool/` 下生产文件。

## 10. 分阶段交付与验收

1. **契约和出口先行**：固定 snapshot schema/identity/buckets；用 test 下快照验证新增指标通过既有转换路径输出，复用 Scheduler/Worker 各自的已有上报机制。
2. **Histogram 与 Reporter**：完成 shared API、读取/差分/选主/state；使用测试文件完成整个累计快照回流闭环。
3. **DramStore 打点**：先 Task/Request/entry/时延，再连接恢复指标，最后评估资源 Gauge；按真实状态机结算。
4. **联调与性能验证**：接同事的真实快照和 DramStore 请求，验证数值回退、轮转、一次选主、部分失败及多 endpoint 查询行为。

测试代码、fixtures、fake 和测试构建支持均位于 `test/` 路径，包括已有 `ucm/shared/test/`、`ucm/store/test/`。真实 Ascend/外部服务验证仅放在显式 opt-in integration target；本次设计阶段不运行硬件测试。

必要测试集合：

- Histogram observation 与 bucket 导入混合后，count/sum/buckets 正确；非法批次不被当作合法导入；并发导入/drain 不丢计数。
- 使用第 6.4.1 节固定样例验证累计快照差分为 `[2,3,1,0]`、count=6、sum=1900 us；最终出口对应 le 增量为 `[2,5,6,6]`、sum 增量为 0.0019 seconds。
- Histogram 的首次 baseline、进程重启、空增量、单桶回退、负数/非整数/超范围桶、桶边界相同长度但数值变化、count 不匹配、零 count 非零 sum。
- state 写入和读取往返后保留 Histogram 边界、单位、整数桶和 sum；大整数 Counter 先差分再转换，不能因累计值浮点舍入丢失小增量。
- 同一快照多次读取、跳过中间快照、重置后累计值已超过旧值的推断限制、累计值回退、桶变化、半行/大行/轮转/坏 state。
- 多进程竞争同一共享锁只产生一个 Leader；抢锁失败线程退出且 Leader 停止后不自动接任；锁释放顺序与同进程重复初始化。
- state 写失败时下一轮读旧基线，允许重复回流；故障窗口测试体现 best-effort 边界，不伪造 exactly-once 断言。
- Task 成功、入队拒绝、入队后过期、部分 request 失败、重复 Check/Wait、异常清理；一个 accepted task 只结算一次。
- Request pending 过期、fence 后退休、重复/过期回复、部分 entry 失败、DUMP 重复 key 的逻辑字节含义。
- 保留既有 Scheduler/Worker 采集路径回归；健康数据沿用实际 stats cadence；新增 Histogram seconds 桶和 sum 一致。
- 打包安装后 DramStore 与 pybind 共用同一 libmetrics；不启用 metrics 时业务仍能运行。

压测对比 metrics 关闭/开启时的请求吞吐与延迟、打点 CPU/内存、Reporter 每轮读取量。验收目标是热路径无文件操作、无按历史请求数增长的统计内存、Histogram 导入成本与桶数相关。具体性能阈值基于项目现有基准与硬件确定，不在无实测时承诺固定百分比。

## 11. 相对旧方案的明确修订

- 删除本侧全部 DramPool 进程实现任务，保留对接契约。
- 明确 DramStore Python 接入层新增一个文件 Reporter 线程，参考 YuanRong 的启动、尾读、flock、差分与 state 流程；C++ 业务打点封装不重复创建文件线程。
- 将 YuanRong 的 Counter-only previous/state 扩展到完整 Histogram，补逐桶差分样例、binding 输入形状和最终 le 桶转换边界。
- Leader 仅在启动时选举一次，失败候选退出，不自动接任。
- 沿用 YuanRong 数值回退推断 reset，不增加 instance_id、sequence 或失败重试去重机制。
- Histogram 保留原分布，新增生产导入 API；不重放虚构 observation。
- Scheduler/Worker 各自沿用既有上报路径，不增加同时采集或合并补丁。
- state 原子替换只保证文件完整，不保证数据已经被 Prometheus 接收；端到端语义明确为 best effort。
- 服务端实际字节、客户端逻辑确认字节分开，防止重复 DUMP 被误解释为真实传输。

本设计可以在同事实现 DramPool 内部的同时推进。需要双方先固定的只有快照身份、累计语义、schema/桶边界和宿主机共享路径；消费端测试使用 `test/` 下快照即可独立开展。
