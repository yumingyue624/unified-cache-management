# SQE 报文长度计算

## 通用公式

```
SQE_size = base_header + batch_number × entry_size
         = 64 + N × entry_size

base_header = kSqeDwordCount × 4 = 16 × 4 = 64 字节（所有 opcode 固定）
```

## 每种 opcode 的长度

| opcode | entry_size | 公式 | max N | 最小长度 | 最大长度 |
|---|---|---|---|---|---|
| Store | - | 64（固定） | - | 64 | 64 |
| Retrieve | - | 64（固定） | - | 64 | 64 |
| KeepAlive | - | 64（固定） | - | 64 | 64 |
| BatchStore | 36 | 64 + N×36 | 110 | 100 | 4024 |
| BatchRetrieve | 36 | 64 + N×36 | 110 | 100 | 4024 |
| Delete | 16 | 64 + N×16 | 254 | 80 | 4128 |
| Exist | 16 | 64 + N×16 | 256 | 80 | 4160 |

## key 长度不影响 SQE 大小

key 始终嵌在固定 16 字节的槽位里（Store/Retrieve 在 header 的 dword 12-15；BatchStore entry 在 dword 1-4；Delete/Exist entry 在 dword 0-3）。key_len = 1~16 时 SQE 长度不变。

## 常量来源（ASU kv_protocol.h）

```
kSqeDwordCount        = 16    → base header = 64 B
kBatchEntrySizeBytes   = 36    → BatchStore/BatchRetrieve per entry
kKeyEntrySizeBytes     = 16    → Delete/Exist per entry
kMaxBatchNumber        = 110   → BatchStore/BatchRetrieve max N
kMaxDeleteBatchNumber  = 254   → Delete max N
kMaxExistBatchNumber   = 256   → Exist max N
```
