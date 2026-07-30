# DramStore Simulator 分轮压力与异常测试报告

## 1. 测试环境

- 测试日期：2026-07-28。
- 时区：Asia/Shanghai（UTC+08:00）。
- 主机：`110.138.0.3`，Ascend A2。
- 容器：`codex_drampool_cann851`。
- 隔离检出：`/home/codex/ucm-dramstore-sim-codex-20260728`。
- 代码基线：`0d829736d2286f3250a0e690db7d705c877135b1`。
- CANN/HIXL/HCCL：8.5.1。
- DramPool：device 4。
- DramStore simulator：默认使用 device 5；多 store 测试使用 device 5、6。
- 默认 block：4096 字节，每请求 1 block。
- 所有修改均保存在本地工作树或独立 patch，未 commit、未 push。

A2 与 A3 的 transport 差异只作为临时测试兼容处理，不在本报告中展开实现细节。

## 2. 总体结果

| 类别 | 场景 | 最终结果 |
| --- | --- | --- |
| 第一轮 | PUT 10 / GET 10，连续 3 次 | 3/3 通过 |
| 第二轮 | PUT 100 / GET 100 | 通过 |
| 第三轮 | PUT 1000 / GET 1000 | 通过 |
| 第四轮 | PUT 10000 / GET 10000，timeout 边界 | 5 秒正确超时失败，10 秒通过 |
| 参数与配置 | 14 项非法输入 | 14/14 正确拒绝 |
| 功能边界 | no-op、PUT-only、GET-only、LOOKUP-only | 全部通过 |
| 批量与复用 | 多 block、多 round、混合操作 | 全部通过 |
| 多实例 | 两个 DramStore、device 5/6 | 两个进程全部通过 |
| 端口冲突 | store control、one-sided control、pool control | 全部正确失败并清理 |
| 运行时异常 | 非法 NPU、pool 不可达 | 全部正确失败 |
| 故障注入 | pool 中断、store 中断及恢复 | 失败路径和后续恢复通过 |
| 重复稳定性 | 同一 DramPool 连续 5 次 1000/1000 | 5/5 通过 |
| 内存安全 | ASan 混合操作、多 block、多 round | 通过，0 ASan error |

## 3. 第一轮：PUT 10 / GET 10

### 3.1 时间

- 开始：2026-07-28 17:34:51。
- 结束：2026-07-28 17:51:36。
- 本轮包含首次复现、问题诊断、修复和连续 3 次回归。

### 3.2 参数

```text
--put 10 --get 10
--lookup-exist 0 --lookup-miss 0
--block-size 4096 --block-num 1 --rounds 1
```

`GET 10` 会先执行 10 个 warm-up PUT；正式 round 再执行 10 个 PUT 和 10 个 GET，并逐字节校验 4096 字节数据。

### 3.3 最终结果

```text
RUN_1_STATUS=0
store[0] round 1 passed
dramstore simulation passed

RUN_2_STATUS=0
store[0] round 1 passed
dramstore simulation passed

RUN_3_STATUS=0
store[0] round 1 passed
dramstore simulation passed

PASS_COUNT=3/3
```

日志：

```text
/tmp/dramstore-sim-put10-get10-grace1.log
/tmp/dramstore-sim-put10-get10-grace2.log
/tmp/dramstore-sim-put10-get10-grace3.log
```

### 3.4 发现并修复的真实问题

#### 问题一：独立 simulator 构建没有继承 CMake 依赖

首次编译错误：

```text
fatal error: fmt/format.h: No such file or directory
```

`build_dramstore_sim.sh` 找到了 CMake build tree 中的 `libucm_p2p_transport.so`，但没有使用同一 build tree 中的 fmt、spdlog、zlib 头文件和 Debug 静态库。

修复：

```text
ucm/store/dram/scripts/build_dramstore_sim.sh
```

脚本现在会从 `UCM_P2P_ROOT` 推导 CMake build root，并优先使用对应的 `_deps/fmt-*`、`_deps/spdlog-*`、`_deps/zlib-*`。

#### 问题二：Simulator 关闭前没有协调断开 peer

核心 PUT/GET 已经通过后，Shutdown 报：

```text
DeregisterMem returned 103900
TransportManager::Shutdown failed
```

底层直接原因：

```text
Cannot deregistor memory since it is still bound to comm
Please unbind from all bound comm first.
```

Simulator 在注册内存仍绑定 communicator 时直接 Shutdown。修复为先对 `pool_manager_id` 执行 coordinated disconnect，再关闭 control channel 和 manager。

修复：

```text
ucm/store/dram/tests/dramstore_sim.cpp
```

#### 问题三：`TransportManager::transfers_` 并发数据竞争

TaskWorker 线程执行：

```text
ExecuteAsync()
  -> next_transfer_handle_++
  -> transfers_.emplace(...)
```

CompletionPoller 线程同时执行：

```text
GetStatus()
  -> transfers_.find(...)
  -> transfers_.erase(...)
```

原实现无锁并发读写同一个 `std::unordered_map`，属于 C++ undefined behavior。`emplace()` 触发 rehash 时，另一个线程的 `find()` 可能观察到不一致的 bucket 状态。

实际表现：

```text
CompletionPoller data GetStatus failed, handle=46
CompletionPoller disconnecting peer ...
CompletionPoller recovered peer ...
store[0] task[...] failed: response validation failed
```

HIXL plog 没有 handle 46 对应的底层传输错误，问题位于 manager handle 映射层。

修复：

```text
ucm/transport/p2p/include/core/transport_manager.h
ucm/transport/p2p/src/core/transport_manager.cpp
```

- 新增 `transfers_mutex_`。
- 保护 manager handle 分配、`emplace()`、`find()`、`erase()`。
- Shutdown 清空 transfer 表和重置计数器时使用同一把锁。

后续最大 30000 个阶段请求、5 次重复启动和多 store 并发中均未再次出现 handle 丢失。

## 4. 第二轮：PUT 100 / GET 100

### 4.1 时间

- 开始：2026-07-28 19:25:44。
- 结束：2026-07-28 19:25:56。
- 总耗时：12 秒。

### 4.2 覆盖

- 100 个 warm-up PUT。
- 正式 round：100 PUT、100 GET。
- 100 次 4096 字节完整数据校验。

### 4.3 结果

```text
STATUS=0
store[0] round 1 passed
dramstore simulation passed
```

无 timeout、无数据校验失败、无 server error、无异常 peer recovery。

日志：

```text
/tmp/dramstore-sim-put100-get100-round2.log
/tmp/drampool-put100-get100-round2.log
```

## 5. 第三轮：PUT 1000 / GET 1000

### 5.1 时间

- 开始：2026-07-28 19:26:57。
- 结束：2026-07-28 19:27:09。
- 总耗时：12 秒。

### 5.2 覆盖

- 1000 个 warm-up PUT。
- 正式 round：1000 PUT、1000 GET。
- 1000 次 4096 字节完整数据校验。

### 5.3 结果

```text
STATUS=0
sim_timeout_lines=0
sim_failed_lines=0
server_error_lines=0
store[0] round 1 passed
dramstore simulation passed
```

日志：

```text
/tmp/dramstore-sim-put1000-get1000-round3.log
/tmp/drampool-put1000-get1000-round3.log
```

## 6. 第四轮：PUT 10000 / GET 10000 与 timeout

### 6.1 时间

- 本轮开始：2026-07-28 19:28:09。
- 5 秒 timeout 尝试结束：2026-07-28 19:28:31。
- 60 秒诊断性尝试：2026-07-28 19:29:31 至 19:29:53。
- 10 秒最终验证：2026-07-28 19:31:07 至 19:31:30。
- 本轮最终结束：2026-07-28 19:31:30。

### 6.2 覆盖

- 10000 个 warm-up PUT。
- 正式 round：10000 PUT、10000 GET。
- 10000 次 4096 字节完整数据校验，约 39.06 MiB。

### 6.3 默认 5 秒 timeout

```text
operation.timeout_ms: 5000
STATUS=1
sim_timeout_lines=6572
sim_validation_failed=0
server_error_lines=0
server_recovery_lines=0
store[0] failed tasks: 6572
dramstore simulation failed
```

timeout 处理符合预期：

- Simulator 返回非零，不把部分完成误报为成功。
- 6572 个超时任务被计入 `failed tasks`。
- 没有数据校验失败或 manager handle 丢失。
- coordinated disconnect 执行成功。
- DramPool 的 RequestReceiver、TaskWorker、CompletionPoller、GCThread 均正常停止。

日志：

```text
/tmp/dramstore-sim-put10000-get10000-round4.log
/tmp/drampool-put10000-get10000-round4.log
```

### 6.4 10 秒 timeout

60 秒只作为诊断性尝试；最终使用较小的 10 秒重新验证：

```text
operation.timeout_ms: 10000
STATUS=0
sim_timeout_lines=0
sim_failed_lines=0
sim_validation_failed=0
server_error_lines=0
server_recovery_lines=0
store[0] round 1 passed
dramstore simulation passed
```

结论：当前 A2 测试路径下，5 秒不足、10 秒可稳定完成；没有必要使用 60 秒。

日志：

```text
/tmp/dramstore-sim-put10000-get10000-round4-timeout10s.log
/tmp/drampool-put10000-get10000-round4-timeout10s.log
```

## 7. 参数、配置与数值边界

### 7.1 时间

- 开始：2026-07-28 19:34:04。
- 结束：2026-07-28 19:34:08。

### 7.2 结果

14/14 用例按预期返回参数错误码 2：

| 用例 | 预期 | 实际 |
| --- | ---: | ---: |
| 缺少必填参数 | 2 | 2 |
| 非数字 device | 2 | 2 |
| store port 为 0 | 2 | 2 |
| option 缺少 value | 2 | 2 |
| block size 为 0 | 2 | 2 |
| block num 为 0 | 2 | 2 |
| block num 超过 uint16 | 2 | 2 |
| rounds 为 0 | 2 | 2 |
| 未知 option | 2 | 2 |
| pool endpoint 格式错误 | 2 | 2 |
| config 文件不存在 | 2 | 2 |
| YAML 格式错误 | 2 | 2 |
| pool endpoint 不在配置中 | 2 | 2 |
| store endpoint 不在配置中 | 2 | 2 |

日志：

```text
/tmp/dramstore-negative-*.log
```

未发现错误参数继续进入业务请求或留下进程的问题。

## 8. 功能边界、批量与资源复用

### 8.1 基本边界

时间：2026-07-28 19:35:02 至 19:35:35。

| 场景 | 参数摘要 | 结果 |
| --- | --- | --- |
| no-op | PUT/GET/LOOKUP 全为 0 | 通过 |
| PUT-only | PUT 100，其余 0 | 通过 |
| GET-only | GET 100，依赖 warm-up | 通过 |

三项均为 0 timeout、0 failure、0 server error。

### 8.2 LOOKUP、多 block 与多 round

时间：2026-07-28 19:36:11 至 19:36:50。

| 场景 | 参数摘要 | 结果 |
| --- | --- | --- |
| LOOKUP-only | exist 100、miss 100 | 通过 |
| 混合 4-block | PUT/GET/exist/miss 各 100，block num 4，3 rounds | 3/3 rounds 通过 |
| 10 rounds | PUT 200、GET 200，10 rounds | 10/10 rounds 通过 |

验证了：

- LOOKUP hit/miss response 编解码。
- 单请求多个 segment 的地址和结果索引。
- slot、response buffer、metadata 和 transfer handle 的反复复用。
- 多 round 后已有 key 集合扩展和 GET 数据一致性。

### 8.3 不同 block size

时间：2026-07-28 19:37:26 至 19:37:49。

| 场景 | block size | block num | rounds | 结果 |
| --- | ---: | ---: | ---: | --- |
| 极小多 block | 64 B | 16 | 2 | 通过 |
| 大块多 segment | 65536 B | 4 | 2 | 通过 |

两项均为 0 timeout、0 failure、0 server error，GET 全量数据校验通过。

## 9. 多 DramStore 并发

### 9.1 时间

- 开始：2026-07-28 19:38:34。
- 结束：2026-07-28 19:38:52。

### 9.2 配置

```text
store 0: device 5, control 110.138.0.3:49201, one-sided 110.138.0.3:49301
store 1: device 6, control 110.138.0.3:49202, one-sided 110.138.0.3:49302
PUT 100 / GET 100 / LOOKUP exist 20 / LOOKUP miss 20
block num 2 / rounds 2
```

### 9.3 结果

```text
store[0] round 1 passed
store[0] round 2 passed
store[1] round 1 passed
store[1] round 2 passed
dramstore simulation passed
dramstore simulation passed
```

脚本正确汇总两个子进程退出码；两个 store index 的 key 空间隔离正常；server 0 error。

日志：

```text
/tmp/dramstore-multistore.log
/tmp/drampool-multistore.log
```

## 10. 端口冲突与新增清理问题

### 10.1 初次测试

- 开始：2026-07-28 19:39:40。
- 结束：2026-07-28 19:40:04。

| 被占用端口 | 结果 |
| --- | --- |
| store control 49201 | Simulator 返回 1 |
| store one-sided control 49301 | Simulator 返回 1 |
| DramPool control 49200 | DramPool 返回 1 |

所有用例均未挂死。

### 10.2 发现的问题

store control 端口被占用时：

1. `TransportManager::Init()` 已成功。
2. `TcpMessageChannel::Init()` 失败。
3. Metadata exchange 尚未执行，peer 实际没有建立。
4. Finalize 仅根据 `manager_started_` 判断，错误地执行 coordinated disconnect。

因此在真正的端口冲突错误之外，又产生一条误导性的：

```text
transport manager coordinated disconnect failed
```

### 10.3 修复

在 simulator 中新增独立的 `peer_connected_`：

- 只在 `ExchangeMetadata()` 成功后设为 true。
- Finalize 只在 `peer_connected_ == true` 时执行 disconnect。
- disconnect 后立即清零。

修复文件：

```text
ucm/store/dram/tests/dramstore_sim.cpp
```

### 10.4 回归

- 开始：2026-07-28 19:41:40。
- 结束：2026-07-28 19:41:58。

```text
store control collision:
  STATUS=1
  DISCONNECT_FAILURE_LINES=0

store one-sided control collision:
  STATUS=1
  DISCONNECT_FAILURE_LINES=0
```

原始启动错误被保留，误导性的 disconnect failure 已消失。

## 11. 运行时异常与故障注入

### 11.1 非法 NPU 与 pool 不可达

时间：2026-07-28 19:42:46 至 19:42:52。

```text
device 99:
  STATUS=1
  aclrtSetDevice returned 107001

DramPool 未启动:
  STATUS=1
  metadata exchange ... failed
```

两项均无额外 disconnect failure，且没有残留 simulator 进程。

### 11.2 DramPool 在处理中终止

时间：

- 中断测试开始：2026-07-28 19:43:30。
- Simulator 失败结束：2026-07-28 19:43:49。
- 重启后恢复测试：2026-07-28 19:43:50 至 19:44:01。

在 10000 个 warm-up PUT 处理中终止 DramPool：

```text
INTERRUPT_STATUS=1
timeouts=7979
dramstore simulation failed
```

随后重新启动 DramPool，立即执行 100/100：

```text
RECOVERY_STATUS=0
store[0] round 1 passed
dramstore simulation passed
```

结论：pool 进程失效能转化为 simulator 非零退出；清理后重新启动可恢复正常。

### 11.3 DramStore wrapper 在处理中终止

大队列故障注入会放大 A2 临时同步传输队列的退出耗时，因此最终使用小队列隔离正式脚本行为。

最终验证时间：2026-07-28 19:50:10 至 19:50:25。

```text
INTERRUPTED_STATUS=143
REMAINING_SIM_PROCESSES=0
```

不重启 DramPool，3 秒后使用同一 endpoint 再次启动 simulator：

```text
RECOVERY_STATUS=0
SERVER_ERRORS=0
STALE_CLEANUP_FAILURES=0
store[0] round 1 passed
dramstore simulation passed
```

结论：

- `run_dramstore_sim.sh` 的 SIGTERM trap 能终止并回收子进程。
- DramPool 能接受同 manager endpoint 的新 metadata exchange。
- 没有 stale route cleanup failure。

## 12. 重复稳定性

### 12.1 时间

- 开始：2026-07-28 19:51:31。
- 结束：2026-07-28 19:52:18。

### 12.2 方法

保持同一个 DramPool 进程不重启，连续 5 次启动和关闭 DramStore。每次执行：

```text
PUT 1000
GET 1000
LOOKUP exist 20
LOOKUP miss 20
timeout 10 秒
```

### 12.3 结果

```text
RUN 1: STATUS=0, TIMEOUTS=0, FAILS=0
RUN 2: STATUS=0, TIMEOUTS=0, FAILS=0
RUN 3: STATUS=0, TIMEOUTS=0, FAILS=0
RUN 4: STATUS=0, TIMEOUTS=0, FAILS=0
RUN 5: STATUS=0, TIMEOUTS=0, FAILS=0

PASS_COUNT=5/5
SERVER_ERRORS=0
STALE_CLEANUP_FAILURES=0
RECOVERIES=0
```

日志：

```text
/tmp/dramstore-repeat-stability-1.log
/tmp/dramstore-repeat-stability-2.log
/tmp/dramstore-repeat-stability-3.log
/tmp/dramstore-repeat-stability-4.log
/tmp/dramstore-repeat-stability-5.log
/tmp/drampool-repeat-stability.log
```

## 13. ASan 内存安全回归

### 13.1 时间

- 开始：2026-07-28 19:53:11。
- 结束：2026-07-28 19:53:26。

### 13.2 参数

```text
PUT 500 / GET 500
LOOKUP exist 100 / LOOKUP miss 100
block num 4
rounds 2
```

Simulator 使用：

```text
-fsanitize=address
-fno-omit-frame-pointer
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0
```

### 13.3 结果

```text
STATUS=0
ASAN_ERRORS=0
SIM_FAILURES=0
SERVER_ERRORS=0
store[0] round 1 passed
store[0] round 2 passed
dramstore simulation passed
```

日志：

```text
/tmp/dramstore-asan-build.log
/tmp/dramstore-asan-mixed.log
/tmp/drampool-asan-mixed.log
```

未检测到越界访问、use-after-free 或 double-free。

## 14. 最终结论

1. `TransportManager::transfers_` 无锁并发访问是本轮最重要的正式缺陷；修复后在最大 30000 个阶段请求、多 round、多 store 和 5 次重复启动中保持稳定。
2. Simulator 构建脚本缺少 CMake 依赖继承，已修复并在普通及 ASan 构建中验证。
3. Simulator 的 peer 生命周期原先只有 `manager_started_`，无法区分“manager 已启动”和“peer 已连接”；新增 `peer_connected_` 后，端口冲突和初始化失败的清理路径不再误断连。
4. Timeout 行为已经同时验证失败和成功路径：5 秒会明确失败并完整计数，10 秒可以完成 10000/10000；无需使用过大的 timeout。
5. PUT、GET、LOOKUP hit/miss、不同 block size、多 block、多 round、多 store、进程中断与重连均已覆盖。
6. 10000 次 GET 的全部 4096 字节数据校验通过。
7. 当前没有发现新的数据正确性问题、manager handle 丢失或用户态内存安全问题。

## 15. 本地文件分类

真实修复：

```text
ucm/store/dram/scripts/build_dramstore_sim.sh
ucm/store/dram/tests/dramstore_sim.cpp
ucm/transport/p2p/include/core/transport_manager.h
ucm/transport/p2p/src/core/transport_manager.cpp
```

A2 临时兼容：

```text
dramstore_a2_hixl_host_sync.patch
dramstore_a2_hixl_shutdown_grace.patch
```

测试报告：

```text
dramstore_sim_stress_test_report.md
```

所有文件均为本地未提交状态。
