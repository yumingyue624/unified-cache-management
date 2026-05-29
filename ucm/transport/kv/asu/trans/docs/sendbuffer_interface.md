# SendBuffer 接口设计

## 概述

SendBuffer 管理一块 RDMA 注册的 Host Pinned Memory（DRAM），用于存放打包好的 SQE 包，通过 RDMA SEND 发送给 Server。

**核心特性**：
- 零拷贝：SQE 直接 Pack 到 SendBuffer 内存中
- 不分区：所有 QP 共享一块连续内存，CAS 原子分配
- 两阶段提交：Allocate（预留）→ Submit（确认）
- Reorder Buffer（ROB）：支持乱序完成、按序回收
- CID 由上层传入，SendBuffer 不负责分配

## 接口定义

```cpp
class SendBuffer {
public:
    SendBuffer() = default;
    ~SendBuffer();

    // 申请内存 + RDMA 注册
    // capacity: buffer 总大小（字节）
    // pd: RDMA protection domain（用于 ibv_reg_mr）
    Status Init(std::size_t capacity, ibv_pd* pd);

    // RDMA 取消注册 + 释放内存
    void Destroy();

    // 阶段 1：分配空间（CAS 预留）
    // size: SQE 包大小（字节，4 字节对齐）
    // cid: 上层传入的 CID（来自 SqeRequest.cid）
    // sge: 回填 addr（SendBuffer 中的写入地址）、length、lkey
    // buffer 满时内部 TryReclaim + 自旋等待
    Status Allocate(std::size_t size, std::uint16_t cid, struct ibv_sge& sge);

    // 阶段 2：标记已提交（Pack + post_send 成功后调用）
    void Submit(std::uint16_t cid);

    // 取消分配（Pack 或 post_send 失败时调用）
    void Cancel(std::uint16_t cid);

    // 释放空间（收到响应后调用）
    // Allocate 和 Reclaim 时都会内部调用 TryReclaim
    void Reclaim(std::uint16_t cid);

    // 获取 MR rkey（握手时交换给 Server）
    std::uint32_t GetRkey() const;

private:
    // 内存
    void* base_{nullptr};
    std::size_t capacity_{0};

    // RDMA
    ibv_mr* mr_{nullptr};
    std::uint32_t lkey_{0};
    std::uint32_t rkey_{0};

    // CAS 环形缓冲区（字节偏移）
    std::atomic<std::size_t> submit_tail_{0};
    std::atomic<std::size_t> reclaim_head_{0};

    // ROB（Reorder Buffer）
    struct ReorderEntry {
        std::size_t offset;       // 在 SendBuffer 中的字节偏移
        std::size_t length;       // 包大小（字节）
        bool submitted{false};    // 是否已完成 Pack + post_send
        bool completed{false};    // 是否已收到响应
    };
    static constexpr std::size_t kMaxROBEntries = 65536;
    std::vector<ReorderEntry> rob_;
    std::atomic<std::size_t> rob_head_{0};
    std::atomic<std::size_t> rob_tail_{0};

    // CID → ROB 条目索引（O(1) 查找，大小 65536）
    std::vector<std::size_t> cid_to_rob_;

    // 内部方法
    void* AllocateSpace(std::size_t len);  // CAS 竞争 submit_tail
    void TryReclaim();                      // 从 ROB 头部推进 reclaim_head
};
```

## 调用方使用流程

```cpp
// === 初始化 ===
send_buffer.Init(64 * 1024 * 1024, pd);  // 64MB

// === 发送请求 ===

// 1. 分配空间
struct ibv_sge sge;
Status s = send_buffer.Allocate(sqe_size, req.cid, sge);
// sge.addr   = SendBuffer 中的写入地址
// sge.length = sqe_size
// sge.lkey   = mr->lkey

// 2. Pack SQE 直接写入 SendBuffer（零拷贝）
sqe.Pack(req, reinterpret_cast<std::uint32_t*>(sge.addr));

// 3. RDMA SEND
struct ibv_send_wr wr = {};
wr.wr_id   = req.cid;
wr.sg_list = &sge;
wr.num_sge = 1;
wr.opcode  = IBV_WR_SEND;

struct ibv_send_wr* bad_wr = nullptr;
int ret = ibv_post_send(qp, &wr, &bad_wr);
if (ret != 0) {
    send_buffer.Cancel(req.cid);  // post_send 失败，取消分配
    return Status::Error(...);
}

// 4. 标记已提交
send_buffer.Submit(req.cid);

// === 接收响应后释放 ===

// WRITE 模式：轮询 FlagBuffer WRITE slot
while (slot.cid == 0) { /* spin */ }
send_buffer.Reclaim(slot.cid);

// SEND 模式：poll CQ
cqe = ibv_poll_cq(cq);
send_buffer.Reclaim(cqe.wr_id);  // wr_id 就是 CID
```

## 内部实现要点

### Init

```
1. posix_memalign 分配 capacity 字节的 Host Pinned Memory（512B 对齐）
2. ibv_reg_mr 注册 MR（IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE）
3. 保存 lkey、rkey
4. 初始化 ROB（kMaxROBEntries 个条目）
5. 初始化 cid_to_rob_（65536 个条目）
6. submit_tail_ = 0, reclaim_head_ = 0
```

### AllocateSpace（CAS 原子分配）

```
while (true):
    tail = submit_tail_.load()
    head = reclaim_head_.load()

    // 空间不足
    if (tail + len - head > capacity_) → 返回 nullptr

    offset = tail % capacity_

    // 跨边界处理：当前尾部放不下整个包，跳过尾部 padding
    actual_len = len
    if (offset + len > capacity_):
        actual_len = len + (capacity_ - offset)

    // CAS 竞争 submit_tail_
    if (submit_tail_.compare_exchange_weak(tail, tail + actual_len)):
        return base_ + (tail % capacity_)
    // CAS 失败，重试
```

### Allocate

```
1. TryReclaim()
2. addr = AllocateSpace(size)
3. if (!addr):
       // buffer 满，自旋等待
       while (!addr):
           TryReclaim()
           addr = AllocateSpace(size)
           if (!addr): _mm_pause()
4. rob_idx = rob_tail_.fetch_add(1)
5. rob_[rob_idx] = { offset, size, submitted=false, completed=false }
6. cid_to_rob_[cid] = rob_idx
7. sge.addr = (uint64_t)addr
8. sge.length = size
9. sge.lkey = lkey_
10. return Status::OK()
```

### Submit

```
1. rob_idx = cid_to_rob_[cid]
2. rob_[rob_idx].submitted = true
3. TryReclaim()
```

### Cancel

```
1. rob_idx = cid_to_rob_[cid]
2. rob_[rob_idx].submitted = true   // 标记为已提交，让 TryReclaim 能推进
3. rob_[rob_idx].completed = true   // 标记为已完成，让 TryReclaim 能回收
4. TryReclaim()
```

### Reclaim

```
1. rob_idx = cid_to_rob_[cid]
2. rob_[rob_idx].completed = true
3. TryReclaim()
```

### TryReclaim

```
while (true):
    head = rob_head_.load()
    entry = rob_[head]

    // 头部条目尚未提交或尚未完成，停止
    if (!entry.submitted || !entry.completed): break

    // CAS 推进 rob_head_
    if (!rob_head_.compare_exchange_weak(head, head + 1)): continue

    // CAS 推进 reclaim_head_，释放空间
    old_head = reclaim_head_.load()
    reclaim_head_.compare_exchange_strong(old_head, old_head + entry.length)
```

## 设计决策

| 决策项 | 结论 |
|--------|------|
| CID 管理 | 上层传入，SendBuffer 不负责分配 |
| 内存管理 | submit_tail / reclaim_head 字节偏移，CAS 原子分配 |
| 回收机制 | ROB（Reorder Buffer），乱序完成、按序回收 |
| CID 查找 | vector 直接索引（O(1)），大小 65536 |
| 两阶段提交 | Allocate（预留）→ Submit（确认），支持 Cancel |
| 阻塞策略 | buffer 满时 TryReclaim + 自旋等待 |
| 跨边界处理 | 插入 padding，从头部重新开始 |
| 内存类型 | Host Pinned Memory（DRAM 注册给 NPU NIC） |
| MR 注册 | 独立注册，与 FlagBuffer 分开 |
