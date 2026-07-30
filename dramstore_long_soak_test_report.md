# DramStore Simulator 5–6 小时递增长稳与异常测试报告

## 1. 测试目标

在 Ascend A2/CANN 8.5.1 实机容器中，将既有功能测试扩展为累计约 5–6 小时的分阶段测试。测试从 10–20 分钟短阶段开始，逐步扩展至 30、45、60、90 分钟阶段；每阶段独立保存日志和结论，避免开发机或容器中断导致全部结果丢失。

覆盖方向：

- 长时间 PUT、GET、LOOKUP hit/miss 与全量数据校验。
- 不同 block size、block num、round 和操作比例。
- DramStore 反复连接、断开和资源复用。
- 多 DramStore 并发。
- timeout 成功/失败边界。
- 进程故障、端口冲突、异常退出和恢复。
- ASan 与生命周期问题。
- DramPool RSS、线程数、FD 和错误日志随时间的变化。

## 2. 环境与代码

- 测试开始：2026-07-28 20:19:48 +0800。
- 主机：`110.138.0.3`，Ascend A2。
- 容器：`codex_drampool_cann851`。
- 隔离 checkout：`/home/codex/ucm-dramstore-sim-codex-20260728`。
- 基线：`0d829736d2286f3250a0e690db7d705c877135b1`。
- CANN/HIXL/HCCL：8.5.1。
- 真实修复：构建依赖、coordinated disconnect、`peer_connected_`、`transfers_` 并发锁。
- A2 临时兼容：Host memory 在 worker 中同步执行、disconnect 后 1 秒 grace。
- 所有修改均未 commit、未 push。

审计时容器内存在其他任务的 `ucmstore.test` 进程。本测试不终止该进程，使用独立设备/端口和精确 PID 清理。

## 3. 阶段计划

| 阶段 | 计划时长 | 重点 | 状态 |
| --- | ---: | --- | --- |
| 1 | 10 分钟 | 单 pool、反复 store 生命周期、混合尺寸/轮次/LOOKUP、资源观测 | 完成 |
| 2 | 15 分钟 | 5–10 秒 timeout 边界反复验证 | 完成 |
| 3 | 20 分钟 | 双 DramStore 并发和隔离 | 完成 |
| 4 | 30 分钟 | Pool/store 故障注入、端口冲突与恢复 | 完成 |
| 5 | 45 分钟 | 容量、驱逐、block 和操作比例压力 | 聚焦回归完成 |
| 6 | 60 分钟 | ASan、反复启停和生命周期 | 5 分钟 smoke + 38 分钟扩展完成；按用户要求暂停 |
| 7 | 90 分钟 | 持续稳定负载、资源趋势 | 待运行 |
| 8 | 60 分钟 | 综合 soak/chaos 回归 | 待运行 |

计划累计：330 分钟，约 5.5 小时，不含阶段间诊断和构建时间。

## 4. 阶段 1：10 分钟混合生命周期测试

### 4.1 开始信息

- 开始：2026-07-28 20:19:48 后完成环境审计，阶段任务随后启动。
- 独立端口：control `49400/49401`，one-sided `49500/49501`。
- DramPool device 4，DramStore device 5。
- timeout：10 秒。
- 每次 simulator 退出后复用同一 DramPool。
- 每次记录 DramPool RSS、线程数、FD、退出码和关键错误行。

循环场景：

1. 4096 B、1 block、PUT/GET 1000。
2. 4096 B、4 blocks、混合 PUT/GET/LOOKUP、2 rounds。
3. 64 B、16 blocks、混合操作、2 rounds。
4. 65536 B、4 blocks、混合操作、2 rounds。
5. 4096 B、2 blocks、5 rounds。
6. LOOKUP-heavy，hit/miss 各 1000、3 rounds。

### 4.2 结果

第一次正式尝试在第 4 个循环场景提前停止：

```text
开始：2026-07-28 20:25:03 +0800
结束：2026-07-28 20:25:38 +0800
完成迭代：4
通过：3
失败：1
失败场景：65536 B × 4 blocks，PUT/GET 50，2 rounds
response validation failed：100
DramPool server error：0
```

定向隔离结果：

- 全新单一 65536 block-class Pool：通过。
- 全新多 block-class Pool，顺序 64/4096/65536：通过。
- 全新多 block-class Pool，顺序 65536/4096/64：通过。
- 因此排除“65536 block 本身失败”和“多内存池注册顺序失败”。

根因是 simulator 的 `MakeKey()` 将 `key_seed`、`store_index` 和 `sequence` 先 XOR，再生成 128 位 key。不同 simulator 进程使用连续 seed 时，`seed_a ^ sequence_a` 很容易等于 `seed_b ^ sequence_b`。复用同一 DramPool 时：

1. 新 DUMP 命中旧 key，服务端按 DuplicateKey 返回成功，不覆盖旧数据。
2. 旧 key 对应的块可能来自 64 B 或 4096 B block-class。
3. 后续请求以 65536 B LOAD 同一 key，存储长度和期望数据不一致。
4. 两个 round 的 50 个 GET 全部失败，正好产生 100 条 validation failure。

这不是 A2 transport 差异，而是 simulator 测试 key 命名空间缺陷。修复为：

- key 前 64 位只由 `key_seed` 派生，隔离不同 simulator 进程。
- key 后 64 位由 sequence、store index 和 missing 域派生。

修复后需要重新构建 simulator，先重跑定向复现，再从头重新计满 10 分钟。

修复后定向顺序回归：

```text
开始：2026-07-28 20:35:02 +0800
结束：2026-07-28 20:35:38 +0800
同一多 block-class DramPool
依次执行 4096、4096×4、64×16、65536×4
结果：4/4 通过
validation failure：0
server error：0
线程数：23
FD：19
```

第二次正式尝试连续通过 33 轮，在第 34 轮的第 6 次大块场景触发容量耗尽：

```text
开始：2026-07-28 20:36:19 +0800
结束：2026-07-28 20:41:29 +0800
通过：33
第 34 轮失败
底层首个错误：
StoreBegin: Allocate for size 65536 failed, status -50009,
buffer_pool_65536: no free slots
```

容量计算与结果吻合：

- Pool 总容量 1 GiB。
- 原比例 64/4096/65536 为 1/7/2，大块类获得约 20%，约 3276 个 65536 B slot。
- 每个大块场景新增约 600 个 key。
- 第 6 次大块场景累计需求约 3600 slot，超过 3276。
- `default_evict_ratio=0.0`，NoSpace 后两次驱逐不会回收条目。

这是正确的容量耗尽和失败传播，不是数据静默损坏：DUMP/GET 返回失败，simulator 非零退出；DramPool 没有崩溃。后续将容量耗尽与非零驱逐比例单独作为压力阶段。阶段 1 调整 block-class 比例为 1/4/5，继续使用不同 seed 保持真实数据传输，然后重新计满 10 分钟。

最终正式阶段：

```text
开始：2026-07-28 20:43:49 +0800
结束：2026-07-28 20:53:53 +0800
请求时长：600 秒
实际时长：604 秒
迭代：66
通过：66
非预期失败：0
simulator 关键错误：0
DramPool 关键错误：0
强制结束 DramPool：0
```

六类负载各完成 11 次。资源观测：

```text
DramPool initial RSS: 158212 KiB
DramPool max/final RSS: 296820 KiB
线程数 max/final: 23/23
FD max/final: 19/19
```

RSS 增长与每轮使用不同 seed、持续向 Pool 增加 KV 一致；线程和 FD 没有增长。本阶段不能单独把 RSS 增长判定为泄漏，后续固定 key 长阶段会隔离“有效 KV 容量”与“生命周期泄漏”。

结论：修复 key namespace 后，反复 66 次建立连接、混合 DUMP/LOAD/LOOKUP、coordinated disconnect 均稳定；未再出现 manager handle 丢失、校验失败或 stale route 清理错误。

## 5. 阶段 2：15 分钟 timeout 边界与反复启停

### 5.1 计划

- 每个迭代使用全新 DramPool，避免容量积累影响 timeout。
- 每次执行 PUT 10000 / GET 10000、4096 B、1 block。
- 先依次测试 5、6、7、8、9、10 秒，观察实际边界。
- 后续反复执行 5 秒失败路径与 10 秒成功路径。
- 失败必须是明确 timeout，不能出现数据校验、transfer handle、server error 或挂死。
- 每轮记录 simulator timeout 数、退出码、Pool 退出方式和关键错误。

### 5.2 结果

```text
开始：2026-07-28 20:56:21 +0800
结束：2026-07-28 21:11:27 +0800
请求时长：900 秒
实际时长：906 秒
迭代：40
成功路径：21
预期 timeout 失败：19
非预期失败：0
强制结束 DramPool：0
```

逐点边界：

| timeout | 结果 |
| ---: | --- |
| 5 秒 | 失败，timeout 被完整计数 |
| 6 秒 | 失败，3954 个 timeout |
| 7 秒 | 通过，0 timeout |
| 8 秒 | 通过，0 timeout |
| 9 秒 | 通过，0 timeout |
| 10 秒 | 通过，0 timeout |

后续持续交替 5 秒和 10 秒。5 秒路径的 timeout 数量随当时吞吐在约 4823–11251 之间变化，但以下性质始终稳定：

- 5 秒路径返回非零，并把未完成任务计入 failed tasks。
- 10 秒路径返回 0，完整数据校验通过。
- validation failure 为 0。
- `GetStatus`/manager handle failure 为 0。
- DramPool error 为 0。
- 每轮 DramPool 都能正常 TERM 退出，无 SIGKILL。
- 前一轮大量 timeout 不会污染下一轮新 Pool。

结论：当前环境的实际边界落在 6–7 秒之间；10 秒有足够余量，无需设置很大的 timeout。失败和成功路径在 40 次反复启停中保持一致。

## 6. 阶段 3：20 分钟双 DramStore 并发

### 6.1 计划

- 每轮启动一个新 DramPool。
- 同时启动 store 0/device 5 与 store 1/device 6。
- 两个 store 使用独立 control 和 one-sided endpoint。
- 轮换 64 B、4096 B、65536 B block-class。
- 检查两个子进程退出码、每个 store 的 round、数据校验、server error 和精确 PID 清理。

### 6.2 结果

第一次业务运行实际通过，但测试统计脚本将 grep 方括号多转义了一层，导致 4 条 round pass 被计成 0 并提前停止。修正后对原日志重新统计得到 4，确认是测试统计问题，不是产品问题。

最终阶段：

```text
开始：2026-07-28 21:15:54 +0800
结束：2026-07-28 21:36:05 +0800
请求时长：1200 秒
实际时长：1211 秒
迭代：91
通过：91
非预期失败：0
强制结束 DramPool：0
```

每轮包含：

- 2 个 DramStore simulator 成功标记。
- store 0 和 store 1 各 2 个 round，共 4 个 round pass。
- 新 DramPool 和两个新的 HIXL store 生命周期。

累计：

```text
DramStore 进程：182
round：364
64 B × 16：31 轮
4096 B × 4：30 轮
65536 B × 2：30 轮
```

所有轮次均为 0 timeout、0 validation failure、0 manager handle failure、0 server error。device 5/6 的 manager ID、endpoint、key namespace 和响应没有串扰。

## 7. 阶段 4：30 分钟故障注入与恢复

### 7.1 计划

循环执行以下异常，并在每次异常后立即执行 PUT/GET 100 + LOOKUP 恢复用例：

1. PUT/GET 10000 处理中 SIGKILL 本次 DramPool。
2. 处理中 SIGTERM `run_dramstore_sim.sh` wrapper，检查子进程回收。
3. 占用 store control 端口。
4. 占用 store one-sided 端口。
5. 使用非法 device 99。

每个异常必须满足预期非零退出，不得挂死；恢复用例必须通过。只操作本轮记录的 PID。

### 7.2 结果

在正式阶段前完成 5 场景 smoke，5/5 通过。第一次 wrapper smoke 使用过大的 A2 临时同步传输 backlog，恢复等待被 A2 队列主导；缩小为 100/100 后重新 smoke，wrapper 状态 143、残留 0、同 Pool 恢复成功。这属于 A2 临时兼容测试参数调整，不是产品问题。

正式阶段：

```text
开始：2026-07-28 21:47:47 +0800
结束：2026-07-28 22:18:04 +0800
请求时长：1800 秒
实际时长：1817 秒
故障+恢复迭代：101
通过：101
非预期失败：0
Pool 主动 SIGKILL：21
强制清理：0
```

约 20 个完整五场景循环。每轮都满足：

- primary 异常按预期非零退出。
- Pool 崩溃路径能在内部 deadline 内失败，不挂死。
- wrapper 始终返回 143，子 simulator 残留数为 0。
- control 冲突不再产生误导性的 coordinated disconnect failure。
- one-sided 冲突释放后可重新绑定。
- device 99 返回 `aclrtSetDevice returned 107001`。
- 每个异常后的 PUT/GET 100 + LOOKUP 恢复用例状态均为 0。

结论：进程失效、初始化失败、端口重用和同 endpoint 恢复在 101 次重复中稳定；没有发现 stale route、残留进程或需要 SIGKILL 清理的正常退出。

## 8. 阶段 5：45 分钟容量与驱逐压力

### 8.1 计划

- 保持同一个 1 GiB DramPool 运行 45 分钟。
- block-class：4096 B 和 65536 B，各 50%。
- `default_evict_ratio=0.2`，在容量耗尽后触发真实驱逐。
- 使用不同 seed 持续增加 key，直到并越过两个 block-class 的容量。
- 轮换大块 PUT-only、大块混合、中块 PUT-only、中块混合、LOOKUP-heavy。
- 混合场景通过 warm-up barrier/lookup lease 保护本轮 GET key，检验驱逐时数据正确性。
- 每轮记录 RSS、线程数、FD、server error 和数据校验。

### 8.2 结果

正式 45 分钟计时前先做了容量边界聚焦回归。该诊断过程不计入正式阶段时长。

#### 8.2.1 首次容量边界：发现按错误 block-class 驱逐

```text
开始：2026-07-28 22:23:29 +0800
结束：2026-07-28 22:26:28 +0800
通过：20
失败：第 21 次 large_put_pressure
首个底层错误：buffer_pool_65536: no free slots
```

Pool 日志同时显示 POSITION policy 已经选出并删除了 victim。检查实现后确认：

- `BufferManager` 按 block size 维护彼此独立的 4096/65536 空闲池；
- metadata eviction 只按 shard/position 选 victim，没有接收本次分配失败的目标 block size；
- 65536 池耗尽时可能删除 4096 条目，4096 池得到空槽，但 65536 分配仍然失败；
- 这是生产逻辑缺陷，不是 A2 transport 差异。

真实修复保持在 DramPool metadata/eviction policy 内：eviction API 增加可选 `target_size`，TTL/LRU/POSITION 仅从相同 block size 的候选中选择；`StoreBegin` 在目标大小分配失败时，跨 shard 寻找同大小 victim，直到分配成功或确实无候选。远端重新构建 `drampool` 成功。

#### 8.2.2 测试工具语义修正

真实修复后的第 21 次原失败场景不再出现 `no free slots`。随后的混合场景暴露出 simulator 在允许驱逐时仍把所有历史 warm-up/PUT key 当成永久存在：

- 旧 completion barrier 只检查前 `block_num` 个键，压力下这些键可能被合法驱逐；
- 改为检查全部键后，能保护 GET warm-up key，但 PUT-only 场景会把合法 POSITION 驱逐误报为数据错误；
- 因此增加仅测试用的 `--eviction-aware 1`：业务轮次前通过 LOOKUP lease 保护本轮 GET 使用的 warm-up key，业务完成后不再断言所有历史 PUT key 永久存在；
- PUT/GET/LOOKUP 的业务并发和数据校验没有降低。

A2 上一次性并发提交 50 个前置 lease 请求会使 HIXL 断连卡到外层 180 秒；改为逐批提交后恢复到约 8 秒退出。此项属于 A2 临时测试兼容，不作为生产缺陷展开。

另外发现阶段监控以 `kill -0` 判断后台 shell 完成，但 PID 1 未立即回收 zombie，导致一次约 20 分钟的无效等待。后续完成判据改为同时读取进程状态和阶段 summary；这段等待不计入正式测试时长。

#### 8.2.3 修复后跨容量边界回归

```text
开始：2026-07-28 23:21:14 +0800
结束：2026-07-28 23:24:55 +0800
实际时长：221 秒
迭代：25
通过：25
非预期失败：0
第 21 次 large_put_pressure：通过
第 22 次 large_mixed_leased：通过
Pool error / no free slots：0
初始 RSS：150028 KiB
最大/最终 RSS：194684/194684 KiB
线程 max/final：23/23
FD max/final：19/19
```

结论：按目标 block size 驱逐修复已跨过原第 21 次稳定复现点；紧随其后的租约混合读写也通过，没有校验失败、连接泄漏或 Pool 错误。下面从全新 Pool 开始正式计满 45 分钟。

#### 8.2.4 首次正式阶段提前停止：warm-up 与 lease 之间存在测试前置竞态

```text
开始：2026-07-28 23:26:25 +0800
结束：2026-07-28 23:30:22 +0800
实际时长：237 秒
通过：26
失败：第 27 次 large_mixed_leased
Pool error：0
失败位置：业务轮次前 warmup lease barrier
```

容量已满后，旧 simulator 先并发写入 200 个 warm-up key，再统一 LOOKUP 加 lease。同一批较早完成的 DUMP key 可能在 lease 开始前被后续 DUMP 合法驱逐，因此第 27 次在正式 GET 之前失败。修正为 eviction-aware 模式下每批 warm-up DUMP 成功后立即 LOOKUP 加 lease；只串行化测试数据准备，不改变正式业务轮次并发。

修正后跨原失败点回归：

```text
开始：2026-07-28 23:33:55 +0800
结束：2026-07-28 23:38:30 +0800
实际时长：275 秒
迭代：30
通过：30
非预期失败：0
第 27 次 large_mixed_leased：通过
Pool error / no free slots：0
初始 RSS：147992 KiB
最大/最终 RSS：196484/196484 KiB
线程 max/final：23/23
FD max/final：19/19
```

第一次正式阶段没有计入 45 分钟完成时长；修正并跨过原失败点后，从全新 Pool 重新计时。

#### 8.2.5 第二次正式阶段中间检查点

```text
正式开始：2026-07-28 23:39:36 +0800
约 10 分钟检查：2026-07-28 23:50:16 +0800
已完成迭代：69
通过：69
非预期失败：0
累计 eviction 日志：15
当前 RSS：230612 KiB
线程：23
FD：19
```

该阶段已再次跨过第 21/22 次 block-size 驱逐点和第 27 次 warm-up lease 竞态点。逐迭代 TSV、Pool 日志和 69 份 simulator 日志已落盘；正式阶段继续运行。

20 分钟检查点：

```text
检查时间：2026-07-29 00:00:15 +0800
已完成迭代：135
通过：135
非预期失败：0
累计 eviction 日志：45
当前 RSS：242848 KiB
线程：23
FD：19
```

第 10–20 分钟间 RSS 从 230612 增至 242848 KiB，增量明显低于最初填充容量阶段；线程和 FD 无变化，暂未观察到连接生命周期泄漏。

30 分钟检查点：

```text
检查时间：2026-07-29 00:10:27 +0800
已完成迭代：203
通过：203
非预期失败：0
累计 eviction 日志：75
当前 RSS：246880 KiB
线程：23
FD：19
```

第 20–30 分钟 RSS 仅增加 4032 KiB，继续趋于平台；同一 Pool 已完成 203 次 Store 连接、混合传输和断连，没有 transfer handle、stale route、线程或 FD 泄漏迹象。

#### 8.2.6 约 34 分钟处发现业务轮次内 LOOKUP_EXIST 异常

```text
开始：2026-07-28 23:39:57 +0800
停止：2026-07-29 00:14:22 +0800
实际时长：2065 秒
迭代：229
通过：228
失败：第 229 次 medium_mixed_leased round 1 task[277]
round 2：通过
Pool error：0
线程/FD：23/19
最大/最终 RSS：247144/247144 KiB
```

用与 simulator 相同的 GCC `std::shuffle`、seed 和 300 PUT + 300 GET + 100 hit + 100 miss 请求布局做确定性映射，`task[277]` 对应原始请求 628，即第 29 个 `LOOKUP_EXIST`（warm-up 键 112–115）。这些键在 eviction-aware warm-up 中已写入并加 10 秒 lease，业务轮次内返回不存在不是预期驱逐语义，暂按真实 lease/eviction 并发问题继续调查。

为缩短复现周期，将 4096 block-class 缩到 Pool 的 5%，只重复 medium PUT、medium mixed 和 LOOKUP-heavy。第一次快速复现：

```text
开始：2026-07-29 00:20:17 +0800
结束：2026-07-29 00:25:16 +0800
实际时长：299 秒
迭代：30
通过：30
非预期失败：0
初始/最大/最终 RSS：151568/180756/180756 KiB
线程/FD：23/19
```

5 分钟阴性结果说明问题不是“容量一满就必现”；已增强 simulator 失败日志，后续复现会直接记录 kind、block、返回码或 GET data mismatch。继续在该小池配置运行 30 分钟高密度复现。

30 分钟高密度复现的 10 分钟检查点：

```text
开始：2026-07-29 00:29:05 +0800
检查：2026-07-29 00:39:11 +0800
迭代：60
通过：60
非预期失败：0
累计 eviction 日志：30
当前 RSS：177052 KiB
线程/FD：23/19
```

尚未复现；继续使用同一小池运行，不重启累计状态。

高密度复现在第 116 次复现：

```text
停止：2026-07-29 00:48:28 +0800
实际时长：1160 秒
迭代：116
通过：115
失败：第 116 次 medium_mixed_leased round 1 task[53]
增强错误：response validation failed kind=lookup_exist block=0 result=0
round 2：通过
最近一次 Pool eviction：失败前约 28 秒
线程/FD：23/19
```

失败键刚在本次 simulator warm-up 中写入并加 lease，最近一次 Pool 驱逐早于该键创建，因此排除 lease 到期或同轮 eviction。根因是 simulator `ExecuteRequests()` 的响应槽复用：

- eviction-aware warm-up 每次提交一个 DUMP/LOOKUP；
- 旧实现每次都从 `slots_[0]` 开始，看到 device response 后立即清零并复用同一地址；
- response 在 Store 侧可见时，Pool 发起端的异步 response transfer 可能尚未被 `GetStatus` 收割为终态；
- 旧 response 迟到时可覆盖新 response，产生极低概率的 LOOKUP_EXIST 假阴性；
- simulator 退出前已有 3 秒 completion reap grace，说明同一可见性窗口确实存在，但请求间此前没有防复用机制。

修复为 `ExecuteRequests()` 在全部预分配 slot 间轮转起始位置。eviction-aware 的 600 个顺序 warm-up 请求在 800 个 slot 中不再立即复用地址；正式业务请求数量和并发度不变。该项属于 simulator 真实异步生命周期缺陷，不是 A2 临时 hack。下面用相同 5% 小池重跑至少 25 分钟并跨过原第 116 次复现点。

slot 轮转假设被快速反证：回归第 14 次在 warm-up 内再次出现 `lookup_exist block=0 result=0`，因此已撤回该改动，不能作为真实修复。

临时二次探针在第 23 次捕获：

```text
首次：response validation failed kind=lookup_exist block=0 result=0
等待 10 ms 后同 key 新请求：retry=hit
同一时刻：当前 DUMP 触发 POSITION eviction
```

key 在 10 ms 后自行恢复，证明 metadata 没有真正丢失；这是 A2 高压同步/异步混合 transport 下的短暂 response/完成可见性窗口。A2 临时测试策略：

- 仅 eviction-aware warm-up 数据准备阶段，首次 lease miss 后等待 10 ms 再确认；
- 二次仍 miss 则保持失败，不无限重试；
- simulator 进程间增加可配置 0.5 秒完成收割间隔；
- 正式业务 round 中的 PUT/GET/LOOKUP 不重试，真实业务失败仍会停止阶段。

这些策略属于 A2 临时 hack，与按目标 block size eviction 的生产修复分离。下面在相同 5% 小池上继续运行 30 分钟验证。

10 分钟检查点：

```text
开始：2026-07-29 01:08:38 +0800
检查：2026-07-29 01:19:15 +0800
迭代：61
通过：61
非预期失败：0
A2 warm-up retry-hit：0
当前 RSS：181060 KiB
线程/FD：23/19
```

0.5 秒进程间收割间隔下尚未出现瞬态；正式业务 round 无重试，继续运行。

20 分钟检查点：

```text
检查：2026-07-29 01:28:46 +0800
迭代：118
通过：118
非预期失败：0
A2 warm-up retry-hit：0
第 116 次 medium_mixed_leased：通过
当前 RSS：181124 KiB
线程/FD：23/19
```

已跨过无间隔配置的原第 116 次复现点；继续跑满最后 10 分钟。

23 分钟最后可确认检查点：

```text
检查：2026-07-29 01:31:47 +0800
迭代：136
通过：136
非预期失败：0
A2 warm-up retry-hit：0
当前 RSS：181124 KiB
线程/FD：23/19
```

随后本地监控调用异常阻塞约 5 小时；远端脚本自身有 1800 秒 deadline，不会无限运行，但 2026-07-29 约 06:36 +0800 重新连接时，`110.138.0.3:22` TCP 已不可达。故当前只能确认前 23 分钟结果，最后约 7 分钟及阶段 summary 待开发机恢复后读取。该 5 小时监控端空等不计入有效测试时长。

开发机离线前，所有已确认结果均已逐迭代写入远端独立目录：

```text
/tmp/dramstore-long-soak-20260728/phase5-a2-grace-regression-30m
```

本地报告已保存到第 136 次，不依赖远端最终 summary。

开发机恢复后读取到最终 summary：

```text
开始：2026-07-29 01:08:51 +0800
结束：2026-07-29 01:38:58 +0800
请求/实际时长：1800/1807 秒
迭代：179
通过：179
非预期失败：0
正式业务 critical：0
A2 warm-up retry-hit：0
初始 RSS：151352 KiB
最大/最终 RSS：181164/181164 KiB
线程 max/final：23/23
FD max/final：19/19
强制清理：0
```

结论：0.5 秒 A2 完成收割间隔下，179 次高密度 4096 驱逐/混合/LOOKUP 生命周期全部通过，跨过此前第 14、23、116、229 次复现点；正式业务 round 没有使用重试。阶段结束后无本测试遗留 drampool/simulator。

## 9. 阶段 6：ASan 与生命周期

### 9.1 构建与 A2 限制

ASan simulator 和独立 ASan DramPool 均构建成功，`ldd` 确认链接
`/lib/aarch64-linux-gnu/libasan.so.6`。

双端 ASan smoke 中，ASan DramPool 的 HIXL 在 A2 上建连直接返回 503900，首轮业务未建立，180 秒外层 timeout 后停止；没有 ASan 内存报告。该结果属于 ASan 插桩与 A2/HIXL 兼容限制，不能作为产品业务失败或 ASan 通过。

改为正常 DramPool + ASan simulator 后真实业务恢复。ASan 参数：

```text
abort_on_error=1
halt_on_error=1
detect_leaks=0
```

只关闭 leak 检测以避开 Ascend/CANN 全局运行时分配噪声。

### 9.2 5 分钟 smoke

```text
开始：2026-07-29 09:52:08 +0800
结束：2026-07-29 09:57:18 +0800
请求/实际时长：300/310 秒
迭代：27
通过：27
非预期失败：0
ASan error：0
初始 RSS：151624 KiB
最大/最终 RSS：180900/180900 KiB
线程/FD：23/19
forced cleanup：0
```

覆盖 5% 4096 小池上的 PUT 压力、2-round PUT/GET/LOOKUP mixed、LOOKUP-heavy 和反复 Store 启停。下面扩展运行 55 分钟。

55 分钟扩展的 10 分钟检查点：

```text
开始：2026-07-29 09:58:48 +0800
检查：2026-07-29 10:09:14 +0800
迭代：55
通过：55
非预期失败：0
ASan error：0
当前 RSS：179252 KiB
线程/FD：23/19
```

逐进程 ASan simulator 均正常退出，继续同一 Pool 累计生命周期。

20 分钟检查点：

```text
检查：2026-07-29 10:19:27 +0800
迭代：109
通过：109
非预期失败：0
ASan error：0
当前 RSS：179312 KiB
线程/FD：23/19
```

第 10–20 分钟 RSS 只增加 60 KiB，未观察到 Pool 资源或 ASan simulator 生命周期异常。

30 分钟检查点：

```text
检查：2026-07-29 10:29:41 +0800
迭代：163
通过：163
非预期失败：0
ASan error：0
当前 RSS：179332 KiB
线程/FD：23/19
```

第 20–30 分钟 RSS 仅增加 20 KiB；继续最后 25 分钟。

### 9.3 55 分钟扩展阶段的人工暂停检查点

用户要求在午休前暂停剩余测试，因此本阶段于 2026-07-29 10:37:14 +0800
安全停止。停止只针对本阶段记录的脚本、DramPool 和 simulator PID；未操作容器中的
其他任务。

```text
开始：2026-07-29 09:58:48 +0800
暂停：2026-07-29 10:37:14 +0800
请求/实际时长：3300/2306 秒
完整迭代：201
完整迭代通过：201
产品非预期失败：0
被暂停中断的迭代：1（第 202 次，退出码 125）
ASan error：0
初始/最大 Pool RSS：149560/179652 KiB
最大线程/FD：23/19
强制清理：0
```

第 202 次是在运行中收到停止信号，阶段汇总脚本把退出码 125 机械计入
`unexpected_failures=1`；该项是用户主动暂停造成的未完成样本，不是产品失败。
Pool、simulator 和阶段脚本均已退出，阶段目录中没有遗留运行进程。恢复时从剩余
ASan 时长继续，不必重跑已经完成的 201 次。

## 10. DramPool 优雅退出与存活 DramStore 跨重启专项

### 10.1 目标与测试控制

专项测试于 2026-07-29 14:27:09 +0800 开始，长时间 ASan/chaos 阶段仍按
用户要求暂停到当晚。本专项验证：

1. DramPool 在 DramStore 仍存活时收到 SIGTERM，是否能在 Shutdown 内断开本地
   native HIXL route 并正常退出。
2. 使用相同 manager ID 和 endpoint 重启 DramPool 后，原 DramStore 进程能否
   继续工作。
3. 显式重新交换 metadata 后，旧 peer/route 是否能被替换。
4. 远端正常响应、完全冻结和 SIGKILL 三种状态对 Pool Shutdown 的影响。

只在 `ucm/store/dram/tests/dramstore_sim.cpp` 增加两个 test-only 参数，默认行为
不变，未修改 DramPool 或 transport 生产代码：

```text
--round-interval-ms N
--reexchange-before-round 0|1
```

前者构造确定的无 in-flight 请求窗口；后者在第二轮及以后重新
`ExchangeMetadata()`、清除旧 Pool 的易失 key 视图并重新 warm-up。该动作模拟
真实 DramStore 在 Pool 重启后的恢复握手，不是产品修复。

所有远端日志保存在：

```text
/tmp/dramstore-pool-graceful-restart-20260729
```

### 10.2 存活且正常响应的 Store：Pool 本地 Shutdown 正常

目录：

```text
/tmp/dramstore-pool-graceful-restart-20260729/trial2-responsive-store
```

```text
开始：2026-07-29 14:30:40 +0800
结束：2026-07-29 14:32:06 +0800
旧 Pool SIGTERM：14:30:46.014
旧 Pool 退出：14:30:48.330
旧 Pool 退出耗时：2316 ms
旧 Pool 状态：0
强制清理：0
旧 Pool 错误：0
```

旧 Pool 能在 Store control/HIXL 线程正常响应时完成本地 native route 清理、内存
注销和进程退出；没有 `memory still bound`、deregister 或 disconnect 错误。

但未增加恢复逻辑的同一 Store 在新 Pool 上失败。第一轮通过；第二轮开始后新 Pool
的首个决定性错误为：

```text
transport manager local connect failed protocol=0
peer=110.138.0.3:49711 status=-1
```

随后 Store 的第二、三轮请求全部超时。根因不是旧 native route 阻止重连，而是新
Pool 是全新进程，尚未收到这个 Store 的 metadata，`peers_` 中不存在
`110.138.0.3:49711`。当前 fake Store 只在初始化时交换一次 metadata，不具备
Pool 重启检测和自动重交换能力。

### 10.3 显式 metadata 重交换后的单次恢复

目录：

```text
/tmp/dramstore-pool-graceful-restart-20260729/trial3-explicit-reexchange
```

```text
开始：2026-07-29 14:37:10 +0800
结束：2026-07-29 14:38:22 +0800
旧 Pool 退出耗时：2318 ms
旧/新 Pool 状态：0/0
Store 状态：0
业务 round：3/3 通过
metadata reexchange + recovery warm-up：2/2 通过
Pool 错误：0
Store 错误：0
最终 coordinated disconnect：成功
```

同一个 Store 进程、同一个 TransportManager、同一块已注册设备内存跨越 Pool
重启后恢复成功。该结果证明：

- 旧 Pool 的本地 Shutdown 没有留下阻止新实例连接的 native HIXL 残留。
- `ImportMetadata()` 能替换相同 manager ID 的旧 peer。
- 新 Pool 在收到 Store metadata 后能重新构建 route 并主动 Connect。
- DramPool 数据是易失的，恢复后必须重新 warm-up；不能期待重启前 key 仍存在。

### 10.4 同一 Store 连续跨三次 Pool 重启

目录：

```text
/tmp/dramstore-pool-graceful-restart-20260729/trial4-three-restarts
```

```text
开始：2026-07-29 14:39:39 +0800
结束：2026-07-29 14:40:21 +0800
Pool 重启次数：3
三次旧 Pool 退出耗时：2527/2632/2847 ms
三次旧 Pool 状态：全部 0
三次强制清理：全部 0
同一 Store 业务 round：4/4 通过
metadata reexchange + recovery warm-up：3/3 通过
Pool 错误：0
Store 错误：0
最终 coordinated disconnect：成功
```

因此单次恢复结果不是偶然；同一存活 Store 可以在显式恢复握手后反复跨 Pool
优雅重启。

### 10.5 异常边界：Store 存在但完全不调度

目录：

```text
/tmp/dramstore-pool-graceful-restart-20260729/trial5-unresponsive-store
```

在第一轮完全结束后的 120 秒轮间隔中对整个 Store 进程发送 SIGSTOP，再向 Pool
发送 SIGTERM：

```text
Store 冻结：2026-07-29 14:41:16.016 +0800
Pool SIGTERM：2026-07-29 14:41:16.223 +0800
Store 冻结期间观察：60 秒
60 秒内 Pool 退出：否
Store 恢复：2026-07-29 14:42:16.546 +0800
Pool 退出：2026-07-29 14:42:18.967 +0800
Pool 总退出耗时：62744 ms
Pool 状态：0
Pool 错误日志：0
```

线程采样中 Pool 主线程持续在 futex 等待，一个 transport worker 持续处于
`wait_woken`。结合代码调用关系：

```text
HixlTransport::Shutdown
  -> DisconnectRoute
    -> HixlInstance::Disconnect
      -> HixlInstance::Run
        -> worker: engine.Disconnect(remote, timeout_ms=5000)
        -> caller: future.get()
```

可推断阻塞发生在同步 native HIXL disconnect worker；Store 一恢复调度，调用才
完成。这里有两层分类：

- A2/HIXL 8.5.1 行为：`Disconnect(..., 5000)` 没有在远端进程完全冻结时兑现
  5 秒截止时间，需要在 A3 对照。
- UCM 退出鲁棒性风险：`TransportManager::Shutdown()` 对 native disconnect
  没有外层截止/隔离，一个不返回的 runtime 调用可无限阻塞整个 Pool 优雅退出。

该风险尚未直接修改。安全修复需要明确 HIXL 调用可取消性、worker 生命周期和内存
注销契约，不能用简单 detach 或跳过注销掩盖。

### 10.6 对照：Store 已 SIGKILL

目录：

```text
/tmp/dramstore-pool-graceful-restart-20260729/trial6-store-sigkill
```

```text
Store SIGKILL：2026-07-29 14:44:53.909 +0800
Pool SIGTERM：2026-07-29 14:44:54.116 +0800
Pool 退出：2026-07-29 14:44:56.330 +0800
Pool 退出耗时：2214 ms
Pool 状态：0
强制清理：0
Pool 错误/Disconnect warning：0/0
```

因此长时间阻塞不是“远端不在”造成的，而特指远端进程仍存在、连接仍在内核/HIXL
状态中但完全不调度的情况。

### 10.7 专项最终分类

1. **已证实正常**：DramPool Shutdown 内确实会断开本地已知 native route；正常
   响应或已死亡的 Store 下均在约 2–3 秒退出，无内存绑定错误。
2. **fake Store 能力缺口**：当前 simulator 不自动检测 Pool 重启，也不自动重新
   ExchangeMetadata；新 Pool 因无 peer metadata 拒绝连接。这不是由本次证据证明
   的 DramPool 缺陷。
3. **核心恢复能力已证实**：Store 显式 reexchange 并重新 warm-up 后，连续三次
   Pool 重启全部恢复。
4. **新增鲁棒性风险**：远端进程完全冻结时，A2/HIXL native Disconnect 可超过
   配置 timeout，UCM Shutdown 缺少外层截止保护。此项与正常 coordinated
   disconnect 问题分开记录。
5. DramPool 仍没有在 Shutdown 前逐 peer 执行双方 coordinated disconnect；当前
   本地 native cleanup 足以支持已验证的正常退出和显式恢复，但不会替仍存活的
   DramStore 完成重启检测与 metadata 重交换。

剩余长时间 ASan、持续负载和综合 chaos 阶段继续暂停，等待用户当晚下班后的恢复
指令。

## 11. 2026-07-29 晚间恢复与新增配置矩阵

### 11.1 恢复审计与 ASan 剩余阶段

用户于 2026-07-29 18:02 +0800 恢复长稳测试，并新增 YAML 多值矩阵及更广泛异常
场景要求。恢复审计结果：

```text
容器：codex_drampool_cann851
CANN/HIXL/HCCL：8.5.1/8.5.1/8.5.1
隔离 checkout：/home/codex/ucm-dramstore-sim-codex-20260728
基线：0d829736d2286f3250a0e690db7d705c877135b1
设备 4/5/6/7：无 NPU 进程
本测试遗留进程：0
其他任务设备：0–3，未操作
```

暂停前 ASan 扩展完成 2306/3300 秒，因此从相同基线补跑 1020 秒：

```text
开始：2026-07-29 18:03 +0800
目录：/tmp/dramstore-long-soak-20260728/phase6-asan-sim-resume-17m
Pool：正常构建
Simulator：ASan 构建
ASAN_OPTIONS：abort_on_error=1:halt_on_error=1:detect_leaks=0
负载：4096 小池 PUT 压力、混合 PUT/GET/LOOKUP、LOOKUP-heavy
状态：运行中
```

`detect_leaks=0` 只排除 CANN/ACL 全局 runtime 分配器噪声；地址越界、
use-after-free、double-free 等仍为 fail-fast。

补跑完成结果：

```text
结束：2026-07-29 18:19:44 +0800
请求/实际时长：1020/1021 秒
迭代：89
通过：89
异常失败：0
ASan/UBSan 关键字：0
初始/最大/最终 Pool RSS：151360/180928/180928 KiB
最大/最终线程：23/23
最大/最终 FD：19/19
强制清理：0
```

连同暂停前已经完整结束的 201 次迭代，本阶段共有 **290 次完整迭代通过**；
暂停前的 2306 秒与本次 1021 秒合计 3327 秒，已经覆盖原计划的 3300 秒。
暂停时被人工终止的旧第 202 次不计入通过或产品失败。

### 11.2 YAML 非法值与格式拒绝矩阵

```text
开始：2026-07-29 18:10:47 +0800
结束：2026-07-29 18:11:02 +0800
目录：/tmp/dramstore-long-soak-20260728/phase7-yaml-invalid
用例：44
正确拒绝：44
非法配置被接受或启动卡死：0
进程崩溃：0
单例拒绝耗时：约 0.32–0.35 秒
汇总：phase7-yaml-invalid/invalid-summary.json
```

覆盖范围：

- `device_ids` 空、重复、负数和错误列表格式；
- request/completion queue 小于下限，idle wait、pending depth 为零；
- flag buffer 容量/槽大小为零、槽数量不足、uint64 溢出；
- GC 开启但 interval 为零；
- lease/evict period 为零或超过 int64，evict ratio 为负数、超过 1、
  `NaN`、`Inf`；
- operation timeout 为零和 uint32 溢出；
- logger level、目录、轮转文件数和大小非法；
- 不支持的 eviction policy；
- two-sided/one-sided endpoint 重复、角色重叠、缺字段、缺本地 endpoint、
  端口越界；
- 必填键缺失、未知键、重复键、tab 缩进、引号不匹配、错误布尔值和负 unsigned。

本组没有发现新的产品问题。所有非法输入均在硬件 transport 初始化前失败，
错误路径稳定，未污染正在并行执行的 ASan 阶段。

### 11.3 YAML 合法值真实运行矩阵与两项修复

首次矩阵：

```text
开始：2026-07-29 18:21:18 +0800
结束：2026-07-29 18:25:09 +0800
目录：/tmp/dramstore-long-soak-20260728/phase7-yaml-valid
业务通过：14/17
```

覆盖了最小/小型/非对称 queue、1 us 与 500 ms receiver idle wait、最小 flag
buffer、1/4096 字节 slot、GC 关闭与 1 ms interval、TTL/POSITION 互换、1 ms
metadata 时间、evict ratio 1、operation timeout 100 ms、trace/critical 日志、
最小日志轮转和双 transport device。

首次的三项表面失败经分类后为：

1. **真实 DramPool 配置校验缺口**：`flag_buffer.slot_size_bytes=1` 被接受，
   Pool 到达 ready，但所有 KV opcode 的最小响应均至少为 2 字节。Pool 日志为：

   ```text
   TaskWorker ProcessOneRequest failed:
   DUMP response exceeds configured flag buffer slot size
   ```

   TaskWorker 无法构造响应，Store 最终超时。修复是在 YAML 校验阶段拒绝小于
   `kMinimumFlagBufferSlotSizeBytes` 的值，并增加参数化单测；没有改业务路径。

2. `operation.timeout_ms=100` 是预期 timeout 边界：A2 同步兼容路径在 100 ms
   内未完成，Pool 正确执行 timeout、断连、重连，Simulator 按预期失败。这不是
   产品缺陷，修正了矩阵 verdict。

3. `logger.level=critical` 会正确抑制 INFO 级 ready 日志，最初 harness 因依赖
   ready 文本而误判。改为确认进程持续存活后执行真实业务，不改产品。

此外，首次 `device_ids: [4,6]` 虽然 PUT/GET/LOOKUP 通过，但严格检查退出日志时
发现三个：

```text
[Transport][HIXL] unregister memory failed:
DeregisterMem(handle=...) returned 103900
DramPool TransportManager shutdown failed
```

无 peer 的仅启动/退出复现仍稳定出现三次错误，排除了业务连接残留。临时取证日志
确认同一块 Host memory 在 device 4 和 6 的 HIXL instance 注册时返回完全相同的
process-global native handle；原实现按 instance 注销，因此每个 handle 被注销两次。

**真实 transport 修复**：`HixlTransport` 保留每个 instance 的 handle 可用性映射，
但在正常 Shutdown、显式 `UnregisterMemory()` 和注册失败回滚中，将相同 native
handle 归并，只由最先注册它的 instance 注销一次。A3 若返回不同 handle，仍逐个
注销，不改变其多 device 行为。临时 handle 观测日志已删除。

修复后完整复测：

```text
非法矩阵：45/45 正确拒绝
非法开始：2026-07-29 18:42:05 +0800
非法结束：2026-07-29 18:42:19 +0800
非法目录：/tmp/dramstore-long-soak-20260728/phase7-yaml-invalid-fixed

合法矩阵：17/17 符合预期
合法开始：2026-07-29 18:42:50 +0800
合法结束：2026-07-29 18:46:12 +0800
合法目录：/tmp/dramstore-long-soak-20260728/phase7-yaml-valid-fixed

slot_size=1 启动拒绝：通过
slot_size=2 PUT/GET/LOOKUP：通过
100 ms timeout + reconnect：符合预期
critical 日志下真实业务：通过
双 device 业务：通过
双 device DeregisterMem 错误：0
双 device TransportManager shutdown 错误：0
强制清理：0
```

## 12. 2026-07-29 晚间持续负载短阶段的提前停止

```text
计划时长：600 秒
开始：2026-07-29 18:48:33 +0800
停止：2026-07-29 18:52:30 +0800
实际时长：237 秒
迭代：21
通过：20
失败：1（tiny_16block）
Pool 强制清理：0
遗留运行进程：0
目录：/tmp/dramstore-long-soak-20260728/phase8-sustained-10m
```

阶段不是监控掉线后被动丢失，而是脚本的 fail-fast 生效。第 21 个新 Store 刚启动，
Pool 连接新 HIXL engine 时连续返回：

```text
[Transport][HIXL] connect failed ... returned 503900
transport manager local connect failed ... status=-1
```

随后该轮请求没有建立 route，Simulator 将未完成任务按 timeout 失败，阶段按首个
非预期失败主动停止。前 20 轮业务均通过；Pool 最终正常 TERM 退出。

该现象与之前 A2/HIXL 8.5.1 快速反复建连/断连时需要进程间完成收割间隔的证据一致；
本次脚本没有设置已验证的 `A2_INTER_ITERATION_GAP_SECONDS=0.5`。因此当前分类为
**A2/HIXL 快速连接 churn 兼容问题**，不是已确认 DramPool/A3 缺陷。需在 A3
无间隔对照后才能改变归类。未完成的持续负载与综合 chaos 按用户要求暂停到
2026-07-30 中午。
