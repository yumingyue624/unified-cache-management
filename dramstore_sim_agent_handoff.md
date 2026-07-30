# DramStore Simulator A2 实机测试交接文档

## 1. 交接目标

本文档用于让只能拿到远端原始代码的 agent，从相同基线开始，复现 DramStore simulator 的构建、功能、压力、timeout、边界和故障注入测试。

交接包分为三类，必须保持分类：

| 类别 | 文件 | 是否可作为真实修复审查 |
| --- | --- | --- |
| 真实修复 | `dramstore_sim_real_fixes.patch` | 是 |
| A2 临时兼容 | `dramstore_a2_hixl_host_sync.patch` | 否，仅限 A2/HIXL 8.5.1 测试 |
| A2 临时兼容 | `dramstore_a2_hixl_shutdown_grace.patch` | 否，仅限 A2/HIXL 8.5.1 测试 |
| 已有测试结果 | `dramstore_sim_stress_test_report.md` | 只读参考 |

不得提交或推送 A2 临时兼容补丁。Ascend A3 上应先完全跳过两个 A2 补丁，只使用真实修复。

补丁 SHA-256：

```text
dramstore_sim_real_fixes.patch
FC750C84918D7B2E170D581D6305814A16E4106FE337C5D1505A7B7F9C5CABE6

dramstore_a2_hixl_host_sync.patch
01D13CF3E130CBB033B041DF524CBB7D65D9243A13E9DF55637C9158C961877A

dramstore_a2_hixl_shutdown_grace.patch
93BB6A7146707E3A6ED3E4757EF58A9153A35B2CD064401A5E15FD6916ACFDFD
```

## 2. 已验证环境

- 测试日期：2026-07-28。
- 时区：Asia/Shanghai，UTC+08:00。
- 远端：`root@110.138.0.3:22`。
- 密码：不写入仓库；通过 `UCM_TEST_SSH_PASSWORD` 环境变量提供。
- 主机硬件：Ascend A2。
- 容器：`codex_drampool_cann851`。
- 容器运行时：CANN 8.5.1、HIXL 8.5.1、HCCL 8.5.1。
- 原始代码基线：`0d829736d2286f3250a0e690db7d705c877135b1`。
- 已验证隔离目录：`/home/codex/ucm-dramstore-sim-codex-20260728`。
- DramPool 设备：4。
- DramStore 设备：5。
- 多 DramStore 时额外使用设备：6。
- 单实例 control endpoint：Pool `110.138.0.3:49200`，Store `110.138.0.3:49201`。
- 单实例 one-sided endpoint：Pool `110.138.0.3:49300`，Store `110.138.0.3:49301`。

不要使用 `nt_p2p_test` 容器。它的 HIXL/HCCL 版本混装，不适合本测试。

## 3. 安全边界

1. 先用 `npu-smi info` 确认设备 4、5、6 空闲；忙碌时换一组空闲设备。
2. 使用独立 checkout，不要 reset 共享工作区。
3. 不要运行宽泛的 `pkill drampool` 或 `pkill dramstore`。只记录并终止本次启动得到的 PID。
4. 端口冲突测试必须使用本次专用端口，不要占用其他人的服务端口。
5. 不提交、不 push A2 临时兼容代码。
6. 每个失败都先保存 simulator、DramPool 和 Ascend plog，再进行清理。

## 4. 从原始代码同步修改

### 4.1 推荐目录与基线检查

在容器中创建或使用一个明确的隔离目录。以下命令中的目录仅用于本任务：

```bash
export TEST_ROOT=/home/codex/ucm-dramstore-sim-handoff
cd "${TEST_ROOT}"
git status --short
git rev-parse HEAD
```

预期 HEAD：

```text
0d829736d2286f3250a0e690db7d705c877135b1
```

如果原始代码不是这个提交，不要强制 reset 共享目录。新建隔离 clone，并先用 `git apply --check` 判断补丁能否直接应用。

### 4.2 需要传入容器的文件

```text
dramstore_sim_real_fixes.patch
dramstore_a2_hixl_host_sync.patch
dramstore_a2_hixl_shutdown_grace.patch
dramstore_sim_stress_test_report.md
dramstore_sim_agent_handoff.md
```

可通过 Paramiko SFTP 上传到宿主机 `/tmp/`，再使用 `docker cp` 放入容器：

```powershell
@'
import paramiko
import os

host = "110.138.0.3"
password = os.environ["UCM_TEST_SSH_PASSWORD"]
files = [
    "dramstore_sim_real_fixes.patch",
    "dramstore_a2_hixl_host_sync.patch",
    "dramstore_a2_hixl_shutdown_grace.patch",
    "dramstore_sim_stress_test_report.md",
    "dramstore_sim_agent_handoff.md",
]

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(host, port=22, username="root", password=password, timeout=15)
sftp = ssh.open_sftp()
for name in files:
    sftp.put(name, "/tmp/" + name)
sftp.close()
for name in files:
    _, stdout, stderr = ssh.exec_command(
        f"docker cp /tmp/{name} codex_drampool_cann851:/tmp/{name}", timeout=30
    )
    if stdout.channel.recv_exit_status() != 0:
        raise RuntimeError(stderr.read().decode("utf-8", errors="replace"))
ssh.close()
'@ | python -
```

### 4.3 补丁应用顺序

真实修复必须先应用：

```bash
cd "${TEST_ROOT}"
git apply --check /tmp/dramstore_sim_real_fixes.patch
git apply /tmp/dramstore_sim_real_fixes.patch
```

仅在 `110.138.0.3` 这台 A2 上继续应用：

```bash
git apply --check /tmp/dramstore_a2_hixl_host_sync.patch
git apply /tmp/dramstore_a2_hixl_host_sync.patch

git apply --check /tmp/dramstore_a2_hixl_shutdown_grace.patch
git apply /tmp/dramstore_a2_hixl_shutdown_grace.patch
```

应用后记录：

```bash
git status --short
git diff --check
git diff --stat
```

## 5. 修改代码说明

### 5.1 真实修复一：独立 simulator 构建依赖

文件：

```text
ucm/store/dram/scripts/build_dramstore_sim.sh
```

原始问题：脚本能定位 CMake build tree 中的 `libucm_p2p_transport.so`，却继续从系统路径寻找 fmt、spdlog、zlib。Debug 构建容器中会出现：

```text
fatal error: fmt/format.h: No such file or directory
```

修复内容：

- 从 `UCM_P2P_ROOT` 推导 CMake build root。
- 使用同一 build tree 中 `_deps/fmt-src`、`_deps/spdlog-src` 的头文件。
- 优先链接 `_deps` 中的 `libfmtd.a`、`libspdlogd.a`、`libz.a`。
- 找不到 build-tree 依赖时仍保留系统 `-lfmt -lspdlog -lz` 回退。

### 5.2 真实修复二：正常退出前 coordinated disconnect

文件：

```text
ucm/store/dram/tests/dramstore_sim.cpp
```

原始问题：simulator 在内存仍绑定 communicator 时直接 Shutdown，底层报：

```text
Cannot deregistor memory since it is still bound to comm
DeregisterMem returned 103900
TransportManager::Shutdown failed
```

修复内容：Finalize 中先对 `pool_manager_id` 调用 HIXL coordinated disconnect，再关闭 control channel 和 manager。

### 5.3 真实修复三：区分 manager 已启动和 peer 已连接

同样修改：

```text
ucm/store/dram/tests/dramstore_sim.cpp
```

原始问题：store control 端口冲突时，manager 已初始化，但 metadata exchange 尚未成功；旧清理逻辑仍尝试断开并不存在的 peer，掩盖原始错误。

修复内容：

- 新增 `peer_connected_`。
- 只在 `ExchangeMetadata()` 成功后置 true。
- Finalize 只在 true 时执行 disconnect。
- disconnect 后置 false。

### 5.4 真实修复四：`TransportManager::transfers_` 数据竞争

文件：

```text
ucm/transport/p2p/include/core/transport_manager.h
ucm/transport/p2p/src/core/transport_manager.cpp
```

并发关系：

```text
TaskWorker:
  ExecuteAsync()
    -> next_transfer_handle_++
    -> transfers_.emplace()

CompletionPoller:
  GetStatus()
    -> transfers_.find()
    -> transfers_.erase()
```

原始代码对同一个 `std::unordered_map` 并发读写，属于 C++ undefined behavior。rehash 或 bucket 更新期间，轮询线程可能找不到刚插入的 manager handle，产生：

```text
CompletionPoller data GetStatus failed, handle=...
response validation failed
```

修复内容：

- 增加 `mutable std::mutex transfers_mutex_`。
- 保护 manager transfer handle 分配和 `emplace()`。
- 保护 `find()`、底层状态查询及完成后的 `erase()`。
- Shutdown 清表和重置 `next_transfer_handle_` 时使用同一把锁。

这是产品真实缺陷，不是 A2/A3 差异。

### 5.5 A2 临时兼容一：Host memory async

补丁：

```text
dramstore_a2_hixl_host_sync.patch
```

A2 + HIXL 8.5.1 上 Host memory 的 native `TransferAsync` 会出现：

```text
GetTransferStatus ... 503900
Memory async copy failed
dst_module_id not find
```

补丁仅对 Host memory 在 HIXL worker 中排队执行 `TransferSync`，上层仍保留异步 handle 和轮询生命周期；Device memory 仍走 native async。

这是 A2 测试环境兼容，不应提交，也不应带到 A3。

### 5.6 A2 临时兼容二：disconnect grace

补丁：

```text
dramstore_a2_hixl_shutdown_grace.patch
```

A2/HIXL 8.5.1 的 disconnect ACK 可能早于服务端 communicator 完全释放。补丁在已连接 peer 的 disconnect 后等待 1 秒，仅用于 A2 测试退出清理。

这是 A2 临时 hack，不属于正式修复。

## 6. 构建

### 6.1 环境和版本

```bash
docker start codex_drampool_cann851
docker exec -it codex_drampool_cann851 bash

source /usr/local/Ascend/cann-8.5.1/set_env.sh
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-8.5.1
export ASCEND_TOOLKIT_HOME=/usr/local/Ascend/cann-8.5.1
export HIXL_HOME=/usr/local/Ascend/cann-8.5.1/aarch64-linux

cat /usr/local/Ascend/cann-8.5.1/share/info/hixl/version.info | head -1
cat /usr/local/Ascend/cann-8.5.1/share/info/hccl/version.info | head -1
npu-smi info
```

两个版本命令都必须输出：

```text
Version=8.5.1
```

### 6.2 CMake 构建

```bash
cd "${TEST_ROOT}"

cmake -S . -B build-dramstore-sim-cann851 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_UCM_STORE=ON \
  -DBUILD_UCM_DRAMPOOL=ON \
  -DBUILD_UCM_ASU=OFF \
  -DBUILD_UNIT_TESTS=OFF \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DASCEND_ROOT=/usr/local/Ascend/cann-8.5.1 \
  -DHIXL_ROOT=/usr/local/Ascend/cann-8.5.1/aarch64-linux \
  -DPython_ROOT_DIR=/usr/local/python3.11.14

cmake --build build-dramstore-sim-cann851 -j4
```

### 6.3 独立 simulator 构建

```bash
cd "${TEST_ROOT}"
export UCM_P2P_ROOT="${TEST_ROOT}/build-dramstore-sim-cann851/ucm/transport/p2p"
export ASCEND_ROOT=/usr/local/Ascend/cann-8.5.1
export HIXL_LIB_DIR=/usr/local/Ascend/cann-8.5.1/aarch64-linux/lib64

bash ucm/store/dram/scripts/build_dramstore_sim.sh
test -x ucm/store/dram/build/dramstore_sim
```

## 7. 基础配置

保存为 `/tmp/dramstore-handoff.yaml`：

```yaml
transport:
  device_ids: [4]
  endpoints:
    - two_sided: "110.138.0.3:49200"
      one_sided: "110.138.0.3:49300"
    - two_sided: "110.138.0.3:49201"
      one_sided: "110.138.0.3:49301"

queue:
  request_depth: 65536
  completion_depth: 65536

request_receiver:
  idle_wait_us: 100

poller:
  pending_depth: 64

flag_buffer:
  capacity_mb: 64
  slot_size_bytes: 64

gc:
  enabled: true
  interval_ms: 1000

metadata:
  periodic_eviction_policy: TTL
  deep_eviction_policy: POSITION
  lease_time_ms: 5000
  default_evict_ratio: 0.0
  evict_period_ms: 31536000000

operation:
  timeout_ms: 10000

logger:
  level: info
  dir: ./logs
  max_files: 10
  max_size_mb: 5
```

10000/10000 的最终成功 timeout 是 10 秒。测试 timeout 失败路径时把它改成 5000，不需要调到很大。

## 8. 启动模板

### 8.1 启动 DramPool

当前分支要求 `--nics` 非空，但 DramPool HIXL 路径实际使用 YAML 中的 device/endpoints。使用一个非空名称即可。

```bash
cd "${TEST_ROOT}"
POOL_BIN="${TEST_ROOT}/build-dramstore-sim-cann851/ucm/store/dram/drampool"
CONFIG=/tmp/dramstore-handoff.yaml
POOL_LOG=/tmp/drampool-handoff.log

"${POOL_BIN}" \
  --addr 110.138.0.3:49200 \
  --nics mlx5_0 \
  --pool-size-gb 1 \
  --kvcache-block-sizes 4096 \
  --kvcache-block-proportions 1 \
  --ttl-minutes 120 \
  --config "${CONFIG}" >"${POOL_LOG}" 2>&1 &
POOL_PID=$!

for attempt in $(seq 1 100); do
  grep -q "DramPool service ready" "${POOL_LOG}" && break
  kill -0 "${POOL_PID}" 2>/dev/null || {
    cat "${POOL_LOG}"
    exit 1
  }
  sleep 0.1
done
grep "DramPool service ready" "${POOL_LOG}"
```

### 8.2 启动单个 DramStore simulator

```bash
cd "${TEST_ROOT}"
export DRAMSTORE_SIM_BIN="${TEST_ROOT}/ucm/store/dram/build/dramstore_sim"

bash ucm/store/dram/scripts/run_dramstore_sim.sh \
  --config /tmp/dramstore-handoff.yaml \
  --pool-control 110.138.0.3:49200 \
  --store-control 110.138.0.3:49201 \
  --devices 5 \
  --key-seed 20260728001 \
  --block-size 4096 \
  --block-num 1 \
  --rounds 1 \
  --put 10 \
  --get 10 \
  --lookup-exist 0 \
  --lookup-miss 0 \
  2>&1 | tee /tmp/dramstore-handoff.log
SIM_STATUS=${PIPESTATUS[0]}
```

成功必须同时满足：

```text
SIM_STATUS=0
store[0] round 1 passed
dramstore simulation passed
```

并确认没有：

```text
timeout
response validation failed
GetStatus failed
TransportManager::Shutdown failed
server error
```

### 8.3 清理

```bash
kill -TERM "${POOL_PID}" 2>/dev/null || true
wait "${POOL_PID}" 2>/dev/null || true
```

只清理记录下来的 `POOL_PID` 和 simulator wrapper PID。

## 9. 必须复现的分轮测试

每轮使用新的日志文件，并记录开始/结束时间。

| 轮次 | 参数 | timeout | 预期 |
| --- | --- | ---: | --- |
| 第一轮 | PUT 10 / GET 10，连续 3 次 | 5 秒或 10 秒 | 3/3 通过 |
| 第二轮 | PUT 100 / GET 100 | 5 秒 | 通过 |
| 第三轮 | PUT 1000 / GET 1000 | 5 秒 | 通过 |
| 第四轮失败路径 | PUT 10000 / GET 10000 | 5 秒 | 非零退出，timeout 被完整计数，无数据校验错误 |
| 第四轮成功路径 | PUT 10000 / GET 10000 | 10 秒 | 通过，0 timeout |

注意：GET 会先执行同数量的 warm-up PUT。因此 10000/10000 实际包含 10000 warm-up PUT、10000 正式 PUT、10000 GET，共 30000 个阶段请求。

## 10. 扩展测试矩阵

### 10.1 参数和配置拒绝

以下 14 项都应返回状态 2，且不能残留进程：

1. 缺少必填参数。
2. device 不是数字。
3. store port 为 0。
4. option 缺少 value。
5. block size 为 0。
6. block num 为 0。
7. block num 为 65536，超过 uint16。
8. rounds 为 0。
9. 未知 option。
10. pool endpoint 格式错误。
11. config 文件不存在。
12. YAML 格式错误。
13. pool endpoint 不在配置中。
14. store endpoint 不在配置中。

### 10.2 功能边界

| 场景 | 参数 |
| --- | --- |
| no-op | PUT/GET/LOOKUP 全为 0 |
| PUT-only | PUT 100，其余 0 |
| GET-only | GET 100，其余 0 |
| LOOKUP-only | exist 100、miss 100 |
| 混合多 block | PUT/GET/exist/miss 各 100，block-num 4，rounds 3 |
| 多 round | PUT 200、GET 200，rounds 10 |
| 极小 block | block-size 64，block-num 16，rounds 2 |
| 大 block | block-size 65536，block-num 4，rounds 2 |

所有成功场景都必须检查 GET 全量数据，而不是只检查退出码。

### 10.3 多 DramStore

在 YAML 中增加：

```yaml
    - two_sided: "110.138.0.3:49202"
      one_sided: "110.138.0.3:49302"
```

执行：

```text
devices: 5,6
PUT 100 / GET 100
LOOKUP exist 20 / miss 20
block-num 2
rounds 2
```

必须看到 store 0 和 store 1 的两个 round 都通过。

### 10.4 端口冲突

分别占用并验证：

- store control `49201`。
- store one-sided `49301`。
- pool control `49200`。

预期：

- 对应进程返回 1。
- 不挂死。
- 不额外出现 `transport manager coordinated disconnect failed`。
- 释放占用端口后可立即正常启动。

### 10.5 非法运行时条件

- 使用 device 99，预期 `aclrtSetDevice returned 107001`，状态 1。
- 不启动 DramPool，预期 metadata exchange 失败，状态 1。
- 两种情况都不得额外报告不存在 peer 的 disconnect failure。

### 10.6 故障注入

1. PUT 10000 warm-up 处理中，只终止本次 `POOL_PID`。
2. simulator 应非零退出并计数 timeout/failure。
3. 重启 DramPool，立即运行 PUT/GET 100，必须恢复。
4. 另测对 `run_dramstore_sim.sh` wrapper 发送 SIGTERM：
   - wrapper 应返回 143。
   - 子 simulator 应全部退出。
   - 不重启 DramPool，等待约 3 秒，用同 endpoint 重启 simulator，应通过。

A2 Host-sync 临时队列在超大 backlog 下会放大 wrapper 退出耗时。这个现象只作为 A2 临时兼容限制，不要记录成产品缺陷；wrapper SIGTERM 测试使用较小队列即可。

### 10.7 重复稳定性

保持同一个 DramPool 不重启，连续运行 5 次：

```text
PUT 1000
GET 1000
LOOKUP exist 20
LOOKUP miss 20
timeout 10 秒
```

预期 5/5 通过，且：

```text
SERVER_ERRORS=0
STALE_CLEANUP_FAILURES=0
RECOVERIES=0
```

### 10.8 ASan

```bash
cd "${TEST_ROOT}"
export UCM_P2P_ROOT="${TEST_ROOT}/build-dramstore-sim-cann851/ucm/transport/p2p"
export ASCEND_ROOT=/usr/local/Ascend/cann-8.5.1
export HIXL_LIB_DIR=/usr/local/Ascend/cann-8.5.1/aarch64-linux/lib64

BUILD_DIR="${TEST_ROOT}/ucm/store/dram/build-asan" \
CXXFLAGS="-fsanitize=address -fno-omit-frame-pointer" \
LDFLAGS="-fsanitize=address" \
bash ucm/store/dram/scripts/build_dramstore_sim.sh
```

使用 ASan binary 执行：

```text
PUT 500 / GET 500
LOOKUP exist 100 / miss 100
block-num 4
rounds 2
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0
```

预期 0 ASan error、0 simulator failure、0 server error。

## 11. 已验证结果基准

上一轮完整结果：

- 10/10 连续 3 次：3/3 通过。
- 100/100：通过。
- 1000/1000：通过。
- 10000/10000 + 5 秒：状态 1，6572 个 timeout，0 validation failure。
- 10000/10000 + 10 秒：通过，0 timeout。
- 非法输入：14/14 正确拒绝。
- 多实例：两个 store、四个 round 全通过。
- 同 pool 五次重启 store：5/5 通过。
- pool 中断和 wrapper 中断后的恢复：通过。
- ASan：0 error。

完整时间、日志路径和每个问题的诊断见：

```text
dramstore_sim_stress_test_report.md
```

## 12. 失败诊断优先级

按以下顺序诊断：

1. 保存 simulator 和 DramPool 首个错误。
2. 检查 `/root/ascend/log/debug/plog/` 中时间对应的首个底层错误。
3. 确认 CANN/HIXL/HCCL 都是 8.5.1。
4. 确认设备和四个 endpoint 未被占用。
5. 确认真实修复已经应用。
6. 在 A2 上确认两个 A2 临时补丁已经应用；在 A3 上确认没有应用。

已知签名：

| 错误 | 判断 |
| --- | --- |
| `GetTransferStatus 503900` + `Memory async copy failed` | A2/HIXL Host native-async 限制 |
| `aclrtHostRegister 107017` | device-visible alias 被错误当作 Host 注册 |
| `aclrtIpcMemGetExportKey 507899` | host-mapped alias 被错误当作 Device 注册 |
| `Connect 503900` | 先看 plog 最早的 HCCL/driver 错误 |
| manager handle 查找失败但 plog 无对应传输错误 | 优先检查 `transfers_` 并发保护是否存在 |

## 13. 交付要求

另一个 agent 完成后至少交付：

- 实际基线 commit。
- 容器名和 CANN/HIXL/HCCL 版本。
- 使用的设备和 endpoint。
- 真实修复与 A2 临时补丁的应用状态。
- 每轮开始/结束时间、命令、退出码和日志路径。
- timeout 失败路径和成功路径。
- 所有异常场景结果。
- 任何新发现必须区分“产品真实问题”和“A2 临时兼容问题”。
- 工作区 `git status --short`。
- 明确声明未 commit、未 push。

## 14. 2026-07-29 长稳/驱逐压力增量交接

### 14.1 当前开发机状态

- 最后可连接时间：2026-07-29 01:31:47 +0800 左右。
- 2026-07-29 约 06:36 +0800 起，`110.138.0.3:22` TCP 不可达。
- 容器内最后运行的阶段有 1800 秒 deadline，理论上已自动结束；恢复后先读取：

```text
/tmp/dramstore-long-soak-20260728/phase5-a2-grace-regression-30m/phase5-summary.txt
/tmp/dramstore-long-soak-20260728/phase5-a2-grace-regression-30m/phase5-iterations.tsv
/tmp/dramstore-long-soak-20260728/phase5-a2-grace-regression-30m/phase5-pool.log
```

- 已确认到第 136 次：136/136，通过；A2 warm-up retry-hit 0；Pool 线程/FD 23/19；RSS 181124 KiB。
- 本地监控端随后异常空等约 5 小时，这段时间不能算有效硬件测试时长。

开发机恢复后最终读取结果：阶段于 2026-07-29 01:38:58 +0800 自动结束，
1807 秒、179/179、业务 critical 0、A2 retry-hit 0、线程/FD 23/19、
RSS 151352→181164 KiB、forced cleanup 0；无本测试遗留进程。

### 14.2 新发现的生产真实缺陷：驱逐没有按 block size 选择

涉及文件：

```text
ucm/store/dram/cc/drampool/eviction_policy.h
ucm/store/dram/cc/drampool/pos_eviction_policy.h
ucm/store/dram/cc/drampool/ttl_eviction_policy.h
ucm/store/dram/cc/drampool/lru_eviction_policy.h
ucm/store/dram/cc/drampool/metadata.h
ucm/store/dram/cc/drampool/metadata.cc
```

原问题：`BufferManager` 为 4096/65536 维护独立池，但 metadata eviction 不知道当前分配失败的目标大小。65536 池耗尽时可能删除 4096 entry，最终仍报：

```text
buffer_pool_65536: no free slots
```

修复：

- eviction API 增加可选 `target_size`；
- TTL/LRU/POSITION 只从相同大小候选中选 victim；
- `StoreBegin` 在 NoSpace 后跨 shard 深度搜索同大小 victim 并重试分配。

该项不是 A2 差异。修复后多次跨过原第 21 次稳定复现点，Pool 无 `no free slots`。

### 14.3 simulator/test harness 真实修正

1. `MakeKey()` 将 `key_seed` 与 `sequence` 分域，避免连续 seed XOR 碰撞。
2. `--eviction-aware 1`：
   - warm-up key 在正式 GET 前加 lease；
   - 允许驱逐时不再错误断言所有历史 PUT key 永久存在；
   - 正式业务 PUT/GET/LOOKUP 并发与校验不降低。
3. validation 失败日志增加 kind、block、result/data mismatch。
4. 压力脚本支持 `MEDIUM_ONLY`、`POOL_PROPORTIONS` 和
   `A2_INTER_ITERATION_GAP_SECONDS`。

### 14.4 A2 临时项，A3 不要直接携带

除原有 Host transfer sync、shutdown grace 外，本轮增加：

- warm-up lease 请求逐批提交，避免 A2 一次性 50 个前置请求导致断连卡到 180 秒；
- eviction-aware warm-up 的首次 lease miss 等 10 ms 做一次确认；
- 二次仍 miss 才失败，不无限重试；
- simulator 进程间可配置 0.5 秒完成收割间隔；
- 正式业务 round 不重试。

证据：首次 `lookup_exist block=0 result=0` 后，相同 key 在 10 ms 后
`retry=hit`；metadata 没有真正丢 key，属于 A2 高压同步/异步混合路径的短暂完成可见性窗口。

### 14.5 已反证、不要应用的改动

`dramstore_sim_response_slot_rotation_fix.patch` 是诊断假设产物，已经从本地和远端源码撤回。相同配置应用后第 14 次仍失败，因此它不是根因修复。保留该 patch 仅用于审计反证过程，不要应用。

### 14.6 新增本地文件

```text
dramstore_long_phase5_eviction.sh
dramstore_sim_full_barrier_fix.patch
dramstore_sim_barrier_sequential_a2.patch
dramstore_sim_eviction_aware_test_mode.patch
dramstore_sim_eviction_warmup_lease_fix.patch
dramstore_sim_validation_diagnostics.patch
dramstore_a2_eviction_prep_grace.patch
dramstore_sim_response_slot_rotation_fix.patch  # 已反证，不应用
dramstore_shuffle_probe.cpp                     # 诊断工具
dramstore_long_soak_test_report.md
```

### 14.7 开发机恢复后的第一组动作

1. 只读获取最后阶段 summary/TSV/log，不要先重启或清理容器。
2. 确认没有本测试遗留的 drampool/simulator；不要终止其他人的 `ucmstore.test`。
3. 将最后 7 分钟结果补进 `dramstore_long_soak_test_report.md`。
4. 若 30 分钟阶段通过，再继续 ASan/lifecycle 和综合 chaos；若失败，先看正式业务
   round 是否失败，warm-up 的单次 retry-hit 只作为 A2 临时统计。
5. 所有修改仍未 commit、未 push。

### 14.8 2026-07-29 10:37 人工暂停状态

用户要求午休前停止剩余测试。已精确停止
`phase6-asan-sim-55m` 的阶段脚本、DramPool 和当前 ASan simulator，
未终止容器中的其他任务。

```text
目录：/tmp/dramstore-long-soak-20260728/phase6-asan-sim-55m
开始：2026-07-29 09:58:48 +0800
暂停：2026-07-29 10:37:14 +0800
实际运行：2306 秒
完整迭代：201/201 通过
中断样本：第 202 次，退出码 125（用户暂停，不是产品失败）
ASan error：0
Pool 最大 RSS：179652 KiB
Pool 最大线程/FD：23/19
强制清理：0
遗留本测试进程：0
```

恢复时先只读复核上述目录和容器进程，再补跑剩余 ASan 时间，之后继续综合
chaos/故障注入阶段。不要把第 202 次计为产品缺陷，也不要重跑已经完成的
201 次。所有代码仍未 commit、未 push。

### 14.9 DramPool 优雅重启与存活 Store 专项

2026-07-29 14:27–14:45 +0800 完成专项。远端总目录：

```text
/tmp/dramstore-pool-graceful-restart-20260729
```

test-only simulator 控制参数：

```text
--round-interval-ms N
--reexchange-before-round 0|1
```

它们只用于构造无 in-flight 窗口及模拟真实 Store 的恢复握手，默认行为不变，不是
DramPool/transport 产品修复。

关键结果：

- `trial2-responsive-store`：旧 Pool SIGTERM 后 2316 ms、状态 0 退出，错误 0；
  未 reexchange 的原 Store 在新 Pool 上失败，首错为新 Pool
  `local connect failed ... peer=...:49711 status=-1`，因为新实例没有 Store
  metadata。
- `trial3-explicit-reexchange`：Pool 退出 2318 ms；同一 Store reexchange、
  recovery warm-up 2/2，业务 3/3，所有进程状态 0。
- `trial4-three-restarts`：同一 Store 连续跨 3 次 Pool 重启；三次 Pool 退出
  2527/2632/2847 ms，4/4 round、3/3 reexchange 全通过，错误 0。
- `trial5-unresponsive-store`：Store 整体 SIGSTOP 时 Pool SIGTERM 60 秒未退出；
  Store SIGCONT 后约 2.4 秒退出，总耗时 62744 ms。线程和代码关系指向 worker
  内同步 `engine.Disconnect(..., timeout_ms=5000)` 未按时返回。分类为 A2/HIXL
  timeout 行为加 UCM Shutdown 无外层截止的鲁棒性风险，尚未贸然修复。
- `trial6-store-sigkill`：Store 已 SIGKILL 时 Pool 2214 ms、状态 0 退出，错误
  和 disconnect warning 均为 0。

结论：Pool Shutdown 会清理本地 native route；正常响应/已死亡 peer 下行为正常。
fake Store 缺少自动 Pool-restart 检测和 metadata 重交换。显式 reexchange 后核心
链路能连续恢复。远端“存在但完全不调度”会让 native disconnect 无界等待，是单独
的鲁棒性风险。

剩余长稳阶段按用户要求暂停到当晚下班后。所有修改仍未 commit、未 push。
