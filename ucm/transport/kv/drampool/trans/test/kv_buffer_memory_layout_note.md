# DRAM Pool Buffer 内存计算

## 每个槽的 stride

| buffer | 实际需要 | 64B 对齐后 stride |
|---|---|---|
| flag | 71 B | 128 （ALIGN_UP(71, 64) = 128） |
| send | 4112 B | 4160 （ALIGN_UP(4112, 64) = 4160） |

128 和 4160 都是 64 的倍数，且 4096 也是 64 的倍数 → 只要 block 起始 4K 对齐，每个 slot 自动 64B 对齐，两个约束同时满足。

## 分配公式

block 起始要 4K 对齐（给 `aclrtHostRegisterV2` 用），所以多分配 `alignment - 1` 用来对齐起点：

```
flag_buffer_alloc  = N × 128  + 4095
send_buffer_alloc  = N × 4160 + 4095
─────────────────────────────────────────
total              = N × 4288 + 8190
```

用法：

```cpp
// 分配
void* raw_flag = aclrtMallocHost(N * 128  + 4095);
void* raw_send = aclrtMallocHost(N * 4160 + 4095);

// 对齐到 4K
uint8_t* flag_base = reinterpret_cast<uint8_t*>(
    (reinterpret_cast<uintptr_t>(raw_flag) + 4095) & ~static_cast<uintptr_t>(4095));
uint8_t* send_base = reinterpret_cast<uint8_t*>(
    (reinterpret_cast<uintptr_t>(raw_send) + 4095) & ~static_cast<uintptr_t>(4095));

// 注册（起始已 4K 对齐）
aclrtHostRegisterV2(flag_base, N * 128,  ...);
aclrtHostRegisterV2(send_base, N * 4160, ...);

// 第 i 个 slot：
// flag_slot[i] = flag_base + i * 128;
// send_slot[i] = send_base + i * 4160;
```

## 100 MB 可支持多少并发

100 MB = 104857600 B

```
N × 4288 + 8190 ≤ 104857600
N ≤ (104857600 - 8190) / 4288
N ≤ 104849410 / 4288
N ≤ 24451.8...
```

**100 MB ≈ 24451 并发**。
