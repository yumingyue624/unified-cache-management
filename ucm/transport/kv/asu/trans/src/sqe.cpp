/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */
#include "sqe.h"
#include "send_buffer.h"
#include <algorithm>
#include <cstring>

namespace UC::ASU {

Status KvStoreSqe::Pack(const SqeRequest& req, std::uint32_t* target)
{
    auto& r = static_cast<const KvStoreRequest&>(req);
    std::memset(target, 0, PackedSize(req));

    // Dword 0: CID[31:16] | Fixed[15:14]=0b11 | Reserved[13:8] | Opcode[7:0]=0x01
    target[0] = (r.cid << 16) | (kFixedBits << 14) | static_cast<std::uint32_t>(SqeOpcode::Store);

    // Dword 1: kv_ns_id[31:0]
    target[1] = r.kv_ns_id;

    // Dword 2: DTYPE[15:13] | DSPEC[12:8] | Reserved[7:0]
    target[2] = ((r.dtype & 0x7) << 13) | ((r.dspec & 0x1F) << 8);

    // Dwords 3-5: reserved (zero)

    // Dword 6-7: DPTR.buffer[63:0] - data buffer address
    target[6] = r.buffer_addr & 0xFFFFFFFFULL;
    target[7] = (r.buffer_addr >> 32) & 0xFFFFFFFFULL;

    // Dword 8: key[0][31:24] = MR_KEY low 8 bits | length[23:0] = buffer length
    target[8] = ((r.mr_key & 0xFF) << 24) | (r.buffer_length & 0xFFFFFF);

    // Dword 9: Type[31:24] = 0x40 | key[3:1][23:0] = MR_KEY high 24 bits
    target[9] =
        (static_cast<std::uint32_t>(DptrType::Standard) << 24) | ((r.mr_key >> 8) & 0xFFFFFF);

    // Dword 10: offset[31:0]
    target[10] = r.offset;

    // Dword 11: LR[31] | Reserved[30:24] | Length[23:0]
    target[11] = (r.lr ? (1U << 31) : 0) | (r.length & 0xFFFFFF);

    // Dwords 12-15: key[15:0] - 16 bytes key, low-byte aligned
    std::size_t key_len = std::min(r.key.size(), static_cast<std::size_t>(16));
    if (key_len > 0) { std::memcpy(&target[12], r.key.data(), key_len); }
    return Status::OK();
}

Status KvStoreSqe::Validate(const std::uint32_t* data) const
{
    
    if (data[6] == 0 && data[7] == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "buffer_addr is zero");
    }
    std::uint32_t buffer_length = data[8] & 0xFFFFFF;
    if (buffer_length == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "buffer_length is zero");
    }
    if (buffer_length % kAlignmentBytes != 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "buffer_length must be 512B aligned");
    }
    if (data[10] % kAlignmentBytes != 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "offset must be 512B aligned");
    }
    std::uint32_t length = data[11] & 0xFFFFFF;
    if (length == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "length is 1-based, must be non-zero");
    }
    bool key_empty = true;
    for (int i = 0; i < 4; ++i) {
        if (data[12 + i] != 0) {
            key_empty = false;
            break;
        }
    }
    if (key_empty) { return Status::Error(StatusCode::INVALID_ARGUMENT, "key is empty"); }
    return Status::OK();
}

Status KvRetrieveSqe::Pack(const SqeRequest& req, std::uint32_t* target)
{
    auto& r = static_cast<const KvRetrieveRequest&>(req);
    std::memset(target, 0, PackedSize(req));

    // Dword 0: CID[31:16] | Fixed[15:14]=0b11 | Reserved[13:8] | Opcode[7:0]=0x02
    target[0] =
        (r.cid << 16) | (kFixedBits << 14) | static_cast<std::uint32_t>(SqeOpcode::Retrieve);

    // Dword 1: kv_ns_id[31:0]
    target[1] = r.kv_ns_id;

    // Dword 2: Reserved[15:0]
    // Dwords 3-5: reserved (zero)

    // Dword 6-7: DPTR.buffer[63:0] - data buffer address
    target[6] = r.buffer_addr & 0xFFFFFFFFULL;
    target[7] = (r.buffer_addr >> 32) & 0xFFFFFFFFULL;

    // Dword 8: key[0][31:24] = MR_KEY low 8 bits | length[23:0] = buffer length
    target[8] = ((r.mr_key & 0xFF) << 24) | (r.buffer_length & 0xFFFFFF);

    // Dword 9: Type[31:24] = 0x40 | key[3:1][23:0] = MR_KEY high 24 bits
    target[9] =
        (static_cast<std::uint32_t>(DptrType::Standard) << 24) | ((r.mr_key >> 8) & 0xFFFFFF);

    // Dword 10: offset[31:0]
    target[10] = r.offset;

    // Dword 11: LR[31] | Reserved[30:24] | Length[23:0]
    target[11] = (r.lr ? (1U << 31) : 0) | (r.length & 0xFFFFFF);

    // Dwords 12-15: key[15:0] - 16 bytes key, low-byte aligned
    std::size_t key_len = std::min(r.key.size(), static_cast<std::size_t>(16));
    if (key_len > 0) { std::memcpy(&target[12], r.key.data(), key_len); }
    return Status::OK();
}

Status KvRetrieveSqe::Validate(const std::uint32_t* data) const
{
    
    if (data[6] == 0 && data[7] == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "buffer_addr is zero");
    }
    std::uint32_t buffer_length = data[8] & 0xFFFFFF;
    if (buffer_length == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "buffer_length is zero");
    }
    if (buffer_length % kAlignmentBytes != 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "buffer_length must be 512B aligned");
    }
    if (data[10] % kAlignmentBytes != 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "offset must be 512B aligned");
    }
    std::uint32_t length = data[11] & 0xFFFFFF;
    if (length == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "length is 1-based, must be non-zero");
    }
    bool key_empty = true;
    for (int i = 0; i < 4; ++i) {
        if (data[12 + i] != 0) {
            key_empty = false;
            break;
        }
    }
    if (key_empty) { return Status::Error(StatusCode::INVALID_ARGUMENT, "key is empty"); }
    return Status::OK();
}

std::size_t KvBatchStoreSqe::PackedSize(const SqeRequest& req) const
{
    auto& r = static_cast<const KvBatchStoreRequest&>(req);
    return (kSqeDwordCount + r.batch_number * kBatchEntryDwordCount) * sizeof(std::uint32_t);
}

Status KvBatchStoreSqe::Pack(const SqeRequest& req, std::uint32_t* target)
{
    auto& r = static_cast<const KvBatchStoreRequest&>(req);
    if (r.batch_number > r.entries.size()) [[unlikely]] {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "batch_number exceeds entries.size()");
    }
    std::memset(target, 0, PackedSize(req));

    // Dword 0: CID[31:16] | Fixed[15:14]=0b11 | Rflag[13] | Reserved[12:8] | Opcode[7:0]=0x45
    target[0] =
        (r.cid << 16) | (kFixedBits << 14) | static_cast<std::uint32_t>(SqeOpcode::BatchStore);
    if (r.rflag) { target[0] |= (1U << 13); }

    // Dword 1: kv_ns_id[31:0]
    target[1] = r.kv_ns_id;

    // Dword 2: DTYPE[15:13] | DSPEC[12:8]
    target[2] = ((r.dtype & 0x7) << 13) | ((r.dspec & 0x1F) << 8);

    // Dword 3-4: Response Buffer Address[63:0]
    target[3] = r.response_buffer_addr & 0xFFFFFFFFULL;
    target[4] = (r.response_buffer_addr >> 32) & 0xFFFFFFFFULL;

    // Dword 5: Response Buffer MR_Key[31:0]
    target[5] = r.response_mr_key;

    // Dword 6-7: DPTR.buffer = 0 (fixed)

    // Dword 8: DPTR.length = Batch Number * 36
    target[8] = r.batch_number * kBatchEntrySizeBytes;

    // Dword 9: DPTR.Type = 0x1
    target[9] = static_cast<std::uint32_t>(DptrType::Batch) << 24;

    // Dword 10: Batch Number
    target[10] = r.batch_number;

    // Dword 11: LR[31]
    if (r.lr) { target[11] |= (1U << 31); }

    // Dwords 12-15: reserved (zero)

    // Pack batch entries
    for (std::size_t i = 0; i < r.batch_number; ++i) {
        PackEntry(r.entries[i], target + kSqeDwordCount + i * kBatchEntryDwordCount);
    }
    return Status::OK();
}

Status KvBatchStoreSqe::Validate(const std::uint32_t* data) const
{
    
    if (data[3] == 0 && data[4] == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "response_buffer_addr is zero");
    }
    std::uint32_t batch_number = data[10] & 0xFFFF;
    if (batch_number == 0 || batch_number > kMaxBatchNumber) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "batch_number must be in range [1, 227]");
    }
    std::uint32_t dptr_length = data[8] & 0xFFFFFF;
    if (dptr_length != batch_number * kBatchEntrySizeBytes) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "DPTR.length must equal batch_number * 36");
    }
    
    return Status::OK();
}

void KvBatchStoreSqe::PackEntry(const KvBatchStoreEntry& entry, std::uint32_t* base)
{
    // Entry Dword 0: offset
    base[0] = entry.offset;

    // Entry Dword 1: key[15:0]
    std::size_t key_len = std::min(entry.key.size(), static_cast<std::size_t>(16));
    if (key_len > 0) { std::memcpy(&base[1], entry.key.data(), key_len); }

    // Entry Dword 5-6: Buffer Address[63:0]
    base[5] = entry.buffer_addr & 0xFFFFFFFFULL;
    base[6] = (entry.buffer_addr >> 32) & 0xFFFFFFFFULL;

    // Entry Dword 7: MR_KEY[0][31:24] = MR_KEY low 8 bits | Length[23:0]
    base[7] = ((entry.mr_key & 0xFF) << 24) | (entry.length & 0xFFFFFF);

    // Entry Dword 8: DPTR.Type = 0x40 | MR_KEY[3:1][23:0] = MR_KEY high 24 bits
    base[8] =
        (static_cast<std::uint32_t>(DptrType::Standard) << 24) | ((entry.mr_key >> 8) & 0xFFFFFF);
}

std::size_t KvBatchRetrieveSqe::PackedSize(const SqeRequest& req) const
{
    auto& r = static_cast<const KvBatchRetrieveRequest&>(req);
    return (kSqeDwordCount + r.batch_number * kBatchEntryDwordCount) * sizeof(std::uint32_t);
}

Status KvBatchRetrieveSqe::Pack(const SqeRequest& req, std::uint32_t* target)
{
    auto& r = static_cast<const KvBatchRetrieveRequest&>(req);
    if (r.batch_number > r.entries.size()) [[unlikely]] {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "batch_number exceeds entries.size()");
    }
    std::memset(target, 0, PackedSize(req));

    // Dword 0: CID[31:16] | Fixed[15:14]=0b11 | Rflag[13] | Reserved[12:8] | Opcode[7:0]=0x46
    target[0] =
        (r.cid << 16) | (kFixedBits << 14) | static_cast<std::uint32_t>(SqeOpcode::BatchRetrieve);
    if (r.rflag) { target[0] |= (1U << 13); }

    // Dword 1: kv_ns_id[31:0]
    target[1] = r.kv_ns_id;

    // Dword 2: Reserved[15:0]
    // Dwords 3-4: Response Buffer Address[63:0]
    target[3] = r.response_buffer_addr & 0xFFFFFFFFULL;
    target[4] = (r.response_buffer_addr >> 32) & 0xFFFFFFFFULL;

    // Dword 5: Response Buffer MR_Key[31:0]
    target[5] = r.response_mr_key;

    // Dword 6-7: DPTR.buffer = 0 (fixed)

    // Dword 8: DPTR.length = Batch Number * 36
    target[8] = r.batch_number * kBatchEntrySizeBytes;

    // Dword 9: DPTR.Type = 0x1
    target[9] = static_cast<std::uint32_t>(DptrType::Batch) << 24;

    // Dword 10: Batch Number
    target[10] = r.batch_number;

    // Dword 11: LR[31]
    if (r.lr) { target[11] |= (1U << 31); }

    // Dwords 12-15: reserved (zero)

    // Pack batch entries
    for (std::size_t i = 0; i < r.batch_number; ++i) {
        PackEntry(r.entries[i], target + kSqeDwordCount + i * kBatchEntryDwordCount);
    }
    return Status::OK();
}

Status KvBatchRetrieveSqe::Validate(const std::uint32_t* data) const
{
    
    if (data[3] == 0 && data[4] == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "response_buffer_addr is zero");
    }
    std::uint32_t batch_number = data[10] & 0xFFFF;
    if (batch_number == 0 || batch_number > kMaxBatchNumber) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "batch_number must be in range [1, 227]");
    }
    std::uint32_t dptr_length = data[8] & 0xFFFFFF;
    if (dptr_length != batch_number * kBatchEntrySizeBytes) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "DPTR.length must equal batch_number * 36");
    }
    
    return Status::OK();
}

void KvBatchRetrieveSqe::PackEntry(const KvBatchRetrieveEntry& entry, std::uint32_t* base)
{
    // Entry Dword 0: offset
    base[0] = entry.offset;

    // Entry Dword 1: key[15:0]
    std::size_t key_len = std::min(entry.key.size(), static_cast<std::size_t>(16));
    if (key_len > 0) { std::memcpy(&base[1], entry.key.data(), key_len); }

    // Entry Dword 5-6: Buffer Address[63:0]
    base[5] = entry.buffer_addr & 0xFFFFFFFFULL;
    base[6] = (entry.buffer_addr >> 32) & 0xFFFFFFFFULL;

    // Entry Dword 7: MR_KEY[0][31:24] = MR_KEY low 8 bits | Length[23:0]
    base[7] = ((entry.mr_key & 0xFF) << 24) | (entry.length & 0xFFFFFF);

    // Entry Dword 8: DPTR.Type = 0x40 | MR_KEY[3:1][23:0] = MR_KEY high 24 bits
    base[8] =
        (static_cast<std::uint32_t>(DptrType::Standard) << 24) | ((entry.mr_key >> 8) & 0xFFFFFF);
}

std::size_t KvDeleteSqe::PackedSize(const SqeRequest& req) const
{
    auto& r = static_cast<const KvDeleteRequest&>(req);
    return (kSqeDwordCount + r.batch_number * kDeleteEntryDwordCount) * sizeof(std::uint32_t);
}

Status KvDeleteSqe::Pack(const SqeRequest& req, std::uint32_t* target)
{
    auto& r = static_cast<const KvDeleteRequest&>(req);
    if (r.batch_number > r.keys.size()) [[unlikely]] {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "batch_number exceeds keys.size()");
    }
    std::memset(target, 0, PackedSize(req));

    // Dword 0: CID[31:16] | Fixed[15:14]=0b11 | Rflag[13] | Reserved[12:8] | Opcode[7:0]=0x08
    target[0] = (r.cid << 16) | (kFixedBits << 14) | static_cast<std::uint32_t>(SqeOpcode::Delete);
    if (r.rflag) { target[0] |= (1U << 13); }

    // Dword 1: kv_ns_id[31:0]
    target[1] = r.kv_ns_id;

    // Dword 2: Reserved[15:0]
    // Dwords 3-4: Response Buffer Address[63:0]
    target[3] = r.response_buffer_addr & 0xFFFFFFFFULL;
    target[4] = (r.response_buffer_addr >> 32) & 0xFFFFFFFFULL;

    // Dword 5: Response Buffer MR_Key[31:0]
    target[5] = r.response_mr_key;

    // Dword 6-7: DPTR.buffer = 0 (fixed)

    // Dword 8: DPTR.length = Batch Number * 16
    target[8] = r.batch_number * kDeleteEntrySizeBytes;

    // Dword 9: DPTR.Type = 0x1
    target[9] = static_cast<std::uint32_t>(DptrType::Batch) << 24;

    // Dword 10: Batch Number
    target[10] = r.batch_number;

    // Dwords 11-15: reserved (zero)

    // Pack delete entries
    for (std::size_t i = 0; i < r.batch_number; ++i) {
        PackEntry(r.keys[i], target + kSqeDwordCount + i * kDeleteEntryDwordCount);
    }
    return Status::OK();
}

Status KvDeleteSqe::Validate(const std::uint32_t* data) const
{
    
    if (data[3] == 0 && data[4] == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "response_buffer_addr is zero");
    }
    std::uint32_t batch_number = data[10] & 0xFFFF;
    if (batch_number == 0 || batch_number > kMaxBatchNumber) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "batch_number must be in range [1, 227]");
    }
    std::uint32_t dptr_length = data[8] & 0xFFFFFF;
    if (dptr_length != batch_number * kDeleteEntrySizeBytes) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "DPTR.length must equal batch_number * 16");
    }
    
    return Status::OK();
}

void KvDeleteSqe::PackEntry(const std::string& key, std::uint32_t* base)
{
    std::size_t key_len = std::min(key.size(), static_cast<std::size_t>(16));
    if (key_len > 0) { std::memcpy(base, key.data(), key_len); }
}

std::size_t KvExistSqe::PackedSize(const SqeRequest& req) const
{
    auto& r = static_cast<const KvExistRequest&>(req);
    return (kSqeDwordCount + r.batch_number * kDeleteEntryDwordCount) * sizeof(std::uint32_t);
}

Status KvExistSqe::Pack(const SqeRequest& req, std::uint32_t* target)
{
    auto& r = static_cast<const KvExistRequest&>(req);
    if (r.batch_number > r.keys.size()) [[unlikely]] {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "batch_number exceeds keys.size()");
    }
    std::memset(target, 0, PackedSize(req));

    // Dword 0: CID[31:16] | Fixed[15:14]=0b11 | Rflag[13] | Reserved[12:8] | Opcode[7:0]=0x0C
    target[0] = (r.cid << 16) | (kFixedBits << 14) | static_cast<std::uint32_t>(SqeOpcode::Exist);
    if (r.rflag) { target[0] |= (1U << 13); }

    // Dword 1: kv_ns_id[31:0]
    target[1] = r.kv_ns_id;

    // Dword 2: Reserved[15:0]
    // Dwords 3-4: Response Buffer Address[63:0]
    target[3] = r.response_buffer_addr & 0xFFFFFFFFULL;
    target[4] = (r.response_buffer_addr >> 32) & 0xFFFFFFFFULL;

    // Dword 5: Response Buffer MR_Key[31:0]
    target[5] = r.response_mr_key;

    // Dword 6-7: DPTR.buffer = 0 (fixed)

    // Dword 8: DPTR.length = Batch Number * 16
    target[8] = r.batch_number * kDeleteEntrySizeBytes;

    // Dword 9: DPTR.Type = 0x1
    target[9] = static_cast<std::uint32_t>(DptrType::Batch) << 24;

    // Dword 10: SC[16] | Batch Number[15:0]
    target[10] = r.batch_number;
    if (r.sc) { target[10] |= (1U << 16); }

    // Dwords 11-15: reserved (zero)

    // Pack exist entries
    for (std::size_t i = 0; i < r.batch_number; ++i) {
        PackEntry(r.keys[i], target + kSqeDwordCount + i * kDeleteEntryDwordCount);
    }
    return Status::OK();
}

Status KvExistSqe::Validate(const std::uint32_t* data) const
{
    
    if (data[3] == 0 && data[4] == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "response_buffer_addr is zero");
    }
    std::uint32_t batch_number = data[10] & 0xFFFF;
    if (batch_number == 0 || batch_number > kMaxBatchNumber) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "batch_number must be in range [1, 227]");
    }
    std::uint32_t dptr_length = data[8] & 0xFFFFFF;
    if (dptr_length != batch_number * kDeleteEntrySizeBytes) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "DPTR.length must equal batch_number * 16");
    }
    
    return Status::OK();
}

void KvExistSqe::PackEntry(const std::string& key, std::uint32_t* base)
{
    std::size_t key_len = std::min(key.size(), static_cast<std::size_t>(16));
    if (key_len > 0) { std::memcpy(base, key.data(), key_len); }
}

Status KvKeepAliveSqe::Pack(const SqeRequest& req, std::uint32_t* target)
{
    auto& r = static_cast<const KvKeepAliveRequest&>(req);
    std::memset(target, 0, PackedSize(req));

    // Dword 0: CID[31:16] | Rflag[13] | Opcode[7:0]=0xF4
    target[0] = (r.cid << 16) | static_cast<std::uint32_t>(SqeOpcode::KeepAlive);
    if (r.rflag) { target[0] |= (1U << 13); }

    // Dword 1-2: Reserved (zero)
    // Dwords 3-4: Response Buffer Address[63:0]
    target[3] = r.response_buffer_addr & 0xFFFFFFFFULL;
    target[4] = (r.response_buffer_addr >> 32) & 0xFFFFFFFFULL;

    // Dword 5: Response Buffer MR_Key[31:0]
    target[5] = r.response_mr_key;

    // Dwords 6-15: reserved (zero)
    return Status::OK();
}

Status KvKeepAliveSqe::Validate(const std::uint32_t* data) const
{
    
    bool rflag = (data[0] >> 13) & 0x1;
    if (rflag && data[3] == 0 && data[4] == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "response_buffer_addr is zero");
    }
    return Status::OK();
}

Status PrepareSend(Sqe& sqe, const SqeRequest& req, std::uint16_t cid,
                   SendBuffer& send_buffer, ScatterGatherEntry& sge)
{
    std::size_t size = sqe.PackedSize(req);
    auto status = send_buffer.Allocate(size, cid, sge);
    if (!status.ok()) { return status; }

    status = sqe.Pack(req, reinterpret_cast<std::uint32_t*>(sge.addr));
    if (!status.ok()) {
        send_buffer.Cancel(cid);
        return status;
    }

    return Status::OK();
}

}  // namespace UC::ASU
