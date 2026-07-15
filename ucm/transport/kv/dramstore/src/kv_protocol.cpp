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
#include "kv_protocol.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace UC::DRAMPOOL {
namespace {

Status Pack4BitResults(void* data, const std::vector<std::uint8_t>& results,
                       const char* protocol)
{
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (results[index] > 0x0FU) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 std::string{protocol} + ": result[" +
                                     std::to_string(index) + "] exceeds 4 bits");
        }
    }

    auto* packed = static_cast<std::uint8_t*>(data);
    for (std::size_t byteIndex = 0; byteIndex < Packed4BitResultSize(results.size());
         ++byteIndex) {
        const std::size_t first = byteIndex * 2U;
        std::uint8_t value = results[first];
        if (first + 1U < results.size()) {
            value = static_cast<std::uint8_t>(value | (results[first + 1U] << 4U));
        }
        packed[byteIndex] = value;
    }
    return Status::OK();
}

void Unpack4BitResults(const void* data, std::size_t resultCount,
                       std::vector<std::uint8_t>& results)
{
    const auto* packed = static_cast<const std::uint8_t*>(data);
    results.resize(resultCount);
    for (std::size_t index = 0; index < resultCount; ++index) {
        results[index] =
            static_cast<std::uint8_t>((packed[index / 2U] >> ((index % 2U) * 4U)) & 0x0FU);
    }
}

Status Pack1BitResults(void* data, const std::vector<std::uint8_t>& results,
                       const char* protocol)
{
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (results[index] > 1U) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 std::string{protocol} + ": result[" +
                                     std::to_string(index) + "] exceeds 1 bit");
        }
    }

    auto* packed = static_cast<std::uint8_t*>(data);
    for (std::size_t byteIndex = 0; byteIndex < Packed1BitResultSize(results.size());
         ++byteIndex) {
        std::uint8_t value = 0;
        const std::size_t first = byteIndex * 8U;
        const std::size_t end = std::min(first + 8U, results.size());
        for (std::size_t index = first; index < end; ++index) {
            value = static_cast<std::uint8_t>(value | (results[index] << (index - first)));
        }
        packed[byteIndex] = value;
    }
    return Status::OK();
}

void Unpack1BitResults(const void* data, std::size_t resultCount,
                       std::vector<std::uint8_t>& results)
{
    const auto* packed = static_cast<const std::uint8_t*>(data);
    results.resize(resultCount);
    for (std::size_t index = 0; index < resultCount; ++index) {
        results[index] =
            static_cast<std::uint8_t>((packed[index / 8U] >> (index % 8U)) & 0x01U);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

KvOpcode PeekOpcode(const void* data)
{
    return static_cast<KvOpcode>(static_cast<const std::uint8_t*>(data)[kOpcodeOffset]);
}

bool IsAllZeroKey(const BlockId& key)
{
    return std::all_of(key.begin(), key.end(),
                       [](std::byte value) { return value == std::byte{}; });
}

const char* ProtocolName(KvOpcode opcode)
{
    switch (opcode) {
        case KvOpcode::None: return "Unknown";
        case KvOpcode::Dump: return "Dump";
        case KvOpcode::Load: return "Load";
        case KvOpcode::Lookup: return "Lookup";
    }
    return "Unknown";
}

void PackHeader(std::uint8_t* out, KvOpcode opcode, std::uint64_t resp_addr,
                std::uint16_t batch_size)
{
    out[kOpcodeOffset] = static_cast<std::uint8_t>(opcode);
    std::memcpy(out + kRespAddrOffset, &resp_addr, sizeof(resp_addr));
    std::memcpy(out + kBatchSizeOffset, &batch_size, sizeof(batch_size));
}

void PackDumpHeader(std::uint8_t* out, KvOpcode opcode, std::uint64_t resp_addr,
                    std::uint16_t batch_size, std::uint32_t ttl)
{
    PackHeader(out, opcode, resp_addr, batch_size);
    std::memcpy(out + kDumpTtlOffset, &ttl, sizeof(ttl));
}

// ===========================================================================
// KvDumpProtocol
// ===========================================================================

// ---- Client side ----

std::size_t KvDumpProtocol::PackedSize(const KvRequest& req) const
{
    const auto& r = static_cast<const KvDumpRequest&>(req);
    return kKvDumpHeaderSize + static_cast<std::size_t>(r.batch_size) * kKvDumpEntrySize;
}

std::size_t KvDumpProtocol::PackedResponseSize(std::size_t result_count) const
{ return Packed4BitResultSize(result_count); }

Status KvDumpProtocol::PackRequest(const KvRequest& req, void* target)
{
    const auto& r = static_cast<const KvDumpRequest&>(req);
    auto status = ValidateRequest(r);
    if (!status.ok()) { return status; }

    auto* out = static_cast<std::uint8_t*>(target);
    PackDumpHeader(out, r.opcode, r.resp_addr, r.batch_size, r.ttl);

    for (std::size_t i = 0; i < r.entries.size(); ++i) {
        const auto& entry = r.entries[i];
        std::uint8_t* base = out + kKvDumpHeaderSize + i * kKvDumpEntrySize;
        std::memcpy(base + kDumpEntryKeyOffset, entry.key.data(), kKvKeySize);
        std::memcpy(base + kDumpEntryAddrOffset, &entry.addr, sizeof(entry.addr));
        std::memcpy(base + kDumpEntryLenOffset, &entry.len, sizeof(entry.len));
        std::memcpy(base + kDumpEntryIdxOffset, &entry.idx, sizeof(entry.idx));
    }
    return Status::OK();
}

Status KvDumpProtocol::UnpackResponse(const void* data, std::uint16_t result_count,
                                      KvResponse& out) const
{
    if (!data) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Dump: UnpackResponse data is null");
    }
    Unpack4BitResults(data, result_count, out.results);
    return Status::OK();
}

Status KvDumpProtocol::ValidateRequest(const KvDumpRequest& req) const
{
    if (req.opcode != KvOpcode::Dump) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Dump: opcode must be Dump");
    }
    auto header_status = ValidateRequestHeader(req);
    if (!header_status.ok()) { return header_status; }

    for (std::size_t i = 0; i < req.entries.size(); ++i) {
        const auto& entry = req.entries[i];
        if (IsAllZeroKey(entry.key)) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Dump: entry[" + std::to_string(i) + "] key is all zero");
        }
        if (entry.addr == 0) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Dump: entry[" + std::to_string(i) + "] addr is zero");
        }
        if (entry.len == 0) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Dump: entry[" + std::to_string(i) + "] len is zero");
        }
    }
    return Status::OK();
}

// ---- Server side ----

Status KvDumpProtocol::UnpackRequest(const void* data, std::size_t size,
                                     std::unique_ptr<KvRequest>& out) const
{
    auto* bytes = static_cast<const std::uint8_t*>(data);
    auto req = std::make_unique<KvDumpRequest>();

    req->opcode = PeekOpcode(data);
    if (req->opcode != KvOpcode::Dump) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Dump: opcode mismatch");
    }

    std::memcpy(&req->resp_addr, bytes + kRespAddrOffset, sizeof(req->resp_addr));
    if (req->resp_addr == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Dump: resp_addr is zero");
    }

    std::memcpy(&req->batch_size, bytes + kBatchSizeOffset, sizeof(req->batch_size));
    if (req->batch_size == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Dump: batch_size is zero");
    }

    std::size_t expected_size =
        kKvDumpHeaderSize + static_cast<std::size_t>(req->batch_size) * kKvDumpEntrySize;
    if (size != expected_size) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Dump: size(" + std::to_string(size) +
                                                               ") != expected(" +
                                                               std::to_string(expected_size) + ")");
    }

    std::memcpy(&req->ttl, bytes + kDumpTtlOffset, sizeof(req->ttl));
    req->entries.resize(req->batch_size);
    for (std::size_t i = 0; i < req->batch_size; ++i) {
        const std::uint8_t* base = bytes + kKvDumpHeaderSize + i * kKvDumpEntrySize;
        std::memcpy(req->entries[i].key.data(), base + kDumpEntryKeyOffset, kKvKeySize);
        if (IsAllZeroKey(req->entries[i].key)) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Dump: entry[" + std::to_string(i) + "] key is all zero");
        }
        std::memcpy(&req->entries[i].addr, base + kDumpEntryAddrOffset, sizeof(std::uint64_t));
        if (req->entries[i].addr == 0) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Dump: entry[" + std::to_string(i) + "] addr is zero");
        }
        std::memcpy(&req->entries[i].len, base + kDumpEntryLenOffset, sizeof(std::uint32_t));
        if (req->entries[i].len == 0) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Dump: entry[" + std::to_string(i) + "] len is zero");
        }
        std::memcpy(&req->entries[i].idx, base + kDumpEntryIdxOffset, sizeof(std::uint32_t));
    }
    out = std::move(req);
    return Status::OK();
}

Status KvDumpProtocol::PackResponse(void* data, const KvResponse& resp) const
{
    if (!data) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Dump: PackResponse data is null");
    }
    return Pack4BitResults(data, resp.results, "Dump");
}

// ===========================================================================
// KvLoadProtocol
// ===========================================================================

// ---- Client side ----

std::size_t KvLoadProtocol::PackedSize(const KvRequest& req) const
{
    const auto& r = static_cast<const KvLoadRequest&>(req);
    return kKvHeaderSize + static_cast<std::size_t>(r.batch_size) * kKvLoadEntrySize;
}

std::size_t KvLoadProtocol::PackedResponseSize(std::size_t result_count) const
{ return Packed4BitResultSize(result_count); }

Status KvLoadProtocol::PackRequest(const KvRequest& req, void* target)
{
    const auto& r = static_cast<const KvLoadRequest&>(req);
    auto status = ValidateRequest(r);
    if (!status.ok()) { return status; }

    auto* out = static_cast<std::uint8_t*>(target);
    PackHeader(out, r.opcode, r.resp_addr, r.batch_size);

    for (std::size_t i = 0; i < r.entries.size(); ++i) {
        const auto& entry = r.entries[i];
        std::uint8_t* base = out + kKvHeaderSize + i * kKvLoadEntrySize;
        std::memcpy(base + kLoadEntryKeyOffset, entry.key.data(), kKvKeySize);
        std::memcpy(base + kLoadEntryAddrOffset, &entry.addr, sizeof(entry.addr));
        std::memcpy(base + kLoadEntryLenOffset, &entry.len, sizeof(entry.len));
        std::memcpy(base + kLoadEntryIdxOffset, &entry.idx, sizeof(entry.idx));
    }
    return Status::OK();
}

Status KvLoadProtocol::UnpackResponse(const void* data, std::uint16_t result_count,
                                      KvResponse& out) const
{
    if (!data) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Load: UnpackResponse data is null");
    }
    Unpack4BitResults(data, result_count, out.results);
    return Status::OK();
}

Status KvLoadProtocol::ValidateRequest(const KvLoadRequest& req) const
{
    if (req.opcode != KvOpcode::Load) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Load: opcode must be Load");
    }
    auto header_status = ValidateRequestHeader(req);
    if (!header_status.ok()) { return header_status; }

    for (std::size_t i = 0; i < req.entries.size(); ++i) {
        const auto& entry = req.entries[i];
        if (IsAllZeroKey(entry.key)) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Load: entry[" + std::to_string(i) + "] key is all zero");
        }
        if (entry.addr == 0) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Load: entry[" + std::to_string(i) + "] addr is zero");
        }
        if (entry.len == 0) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Load: entry[" + std::to_string(i) + "] len is zero");
        }
    }
    return Status::OK();
}

// ---- Server side ----

Status KvLoadProtocol::UnpackRequest(const void* data, std::size_t size,
                                     std::unique_ptr<KvRequest>& out) const
{
    auto* bytes = static_cast<const std::uint8_t*>(data);
    auto req = std::make_unique<KvLoadRequest>();

    req->opcode = PeekOpcode(data);
    if (req->opcode != KvOpcode::Load) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Load: opcode mismatch");
    }

    std::memcpy(&req->resp_addr, bytes + kRespAddrOffset, sizeof(req->resp_addr));
    if (req->resp_addr == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Load: resp_addr is zero");
    }

    std::memcpy(&req->batch_size, bytes + kBatchSizeOffset, sizeof(req->batch_size));
    if (req->batch_size == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Load: batch_size is zero");
    }

    std::size_t expected_size =
        kKvHeaderSize + static_cast<std::size_t>(req->batch_size) * kKvLoadEntrySize;
    if (size != expected_size) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Load: size(" + std::to_string(size) +
                                                               ") != expected(" +
                                                               std::to_string(expected_size) + ")");
    }

    req->entries.resize(req->batch_size);
    for (std::size_t i = 0; i < req->batch_size; ++i) {
        const std::uint8_t* base = bytes + kKvHeaderSize + i * kKvLoadEntrySize;
        std::memcpy(req->entries[i].key.data(), base + kLoadEntryKeyOffset, kKvKeySize);
        if (IsAllZeroKey(req->entries[i].key)) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Load: entry[" + std::to_string(i) + "] key is all zero");
        }
        std::memcpy(&req->entries[i].addr, base + kLoadEntryAddrOffset, sizeof(std::uint64_t));
        if (req->entries[i].addr == 0) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Load: entry[" + std::to_string(i) + "] addr is zero");
        }
        std::memcpy(&req->entries[i].len, base + kLoadEntryLenOffset, sizeof(std::uint32_t));
        if (req->entries[i].len == 0) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Load: entry[" + std::to_string(i) + "] len is zero");
        }
        std::memcpy(&req->entries[i].idx, base + kLoadEntryIdxOffset, sizeof(std::uint32_t));
    }
    out = std::move(req);
    return Status::OK();
}

Status KvLoadProtocol::PackResponse(void* data, const KvResponse& resp) const
{
    if (!data) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Load: PackResponse data is null");
    }
    return Pack4BitResults(data, resp.results, "Load");
}

// ===========================================================================
// KvLookupProtocol
// ===========================================================================

// ---- Client side ----

std::size_t KvLookupProtocol::PackedSize(const KvRequest& req) const
{
    const auto& r = static_cast<const KvLookupRequest&>(req);
    return kKvHeaderSize + static_cast<std::size_t>(r.batch_size) * kKvLookupEntrySize;
}

std::size_t KvLookupProtocol::PackedResponseSize(std::size_t result_count) const
{ return Packed1BitResultSize(result_count); }

Status KvLookupProtocol::PackRequest(const KvRequest& req, void* target)
{
    const auto& r = static_cast<const KvLookupRequest&>(req);
    auto status = ValidateRequest(r);
    if (!status.ok()) { return status; }

    auto* out = static_cast<std::uint8_t*>(target);
    PackHeader(out, r.opcode, r.resp_addr, r.batch_size);

    for (std::size_t i = 0; i < r.entries.size(); ++i) {
        const auto& entry = r.entries[i];
        std::uint8_t* base = out + kKvHeaderSize + i * kKvLookupEntrySize;
        std::memcpy(base + kLookupEntryKeyOffset, entry.key.data(), kKvKeySize);
    }
    return Status::OK();
}

Status KvLookupProtocol::UnpackResponse(const void* data, std::uint16_t result_count,
                                        KvResponse& out) const
{
    if (!data) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Lookup: UnpackResponse data is null");
    }
    Unpack1BitResults(data, result_count, out.results);
    return Status::OK();
}

Status KvLookupProtocol::ValidateRequest(const KvLookupRequest& req) const
{
    if (req.opcode != KvOpcode::Lookup) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Lookup: opcode must be Lookup");
    }
    auto header_status = ValidateRequestHeader(req);
    if (!header_status.ok()) { return header_status; }

    for (std::size_t i = 0; i < req.entries.size(); ++i) {
        if (IsAllZeroKey(req.entries[i].key)) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Lookup: entry[" + std::to_string(i) + "] key is all zero");
        }
    }
    return Status::OK();
}

// ---- Server side ----

Status KvLookupProtocol::UnpackRequest(const void* data, std::size_t size,
                                       std::unique_ptr<KvRequest>& out) const
{
    auto* bytes = static_cast<const std::uint8_t*>(data);
    auto req = std::make_unique<KvLookupRequest>();

    req->opcode = PeekOpcode(data);
    if (req->opcode != KvOpcode::Lookup) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Lookup: opcode mismatch");
    }

    std::memcpy(&req->resp_addr, bytes + kRespAddrOffset, sizeof(req->resp_addr));
    if (req->resp_addr == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Lookup: resp_addr is zero");
    }

    std::memcpy(&req->batch_size, bytes + kBatchSizeOffset, sizeof(req->batch_size));
    if (req->batch_size == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Lookup: batch_size is zero");
    }

    std::size_t expected_size =
        kKvHeaderSize + static_cast<std::size_t>(req->batch_size) * kKvLookupEntrySize;
    if (size != expected_size) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Lookup: size(" + std::to_string(size) +
                                                               ") != expected(" +
                                                               std::to_string(expected_size) + ")");
    }

    req->entries.resize(req->batch_size);
    for (std::size_t i = 0; i < req->batch_size; ++i) {
        const std::uint8_t* base = bytes + kKvHeaderSize + i * kKvLookupEntrySize;
        std::memcpy(req->entries[i].key.data(), base + kLookupEntryKeyOffset, kKvKeySize);
        if (IsAllZeroKey(req->entries[i].key)) {
            return Status::Error(StatusCode::INVALID_ARGUMENT,
                                 "Lookup: entry[" + std::to_string(i) + "] key is all zero");
        }
    }
    out = std::move(req);
    return Status::OK();
}

Status KvLookupProtocol::PackResponse(void* data, const KvResponse& resp) const
{
    if (!data) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "Lookup: PackResponse data is null");
    }
    if (resp.results.empty()) {
        return Status::Error(
            StatusCode::INVALID_ARGUMENT,
            "Lookup: results must not be empty");
    }
    return Pack1BitResults(data, resp.results, "Lookup");
}

// ===========================================================================
// ProtocolManager
// ===========================================================================

ProtocolManager::ProtocolManager()
{
    protocols_[KvOpcode::Dump] = std::make_unique<KvDumpProtocol>();
    protocols_[KvOpcode::Load] = std::make_unique<KvLoadProtocol>();
    protocols_[KvOpcode::Lookup] = std::make_unique<KvLookupProtocol>();
}

KvProtocol* ProtocolManager::GetProtocol(KvOpcode opcode) const
{
    auto it = protocols_.find(opcode);
    return it != protocols_.end() ? it->second.get() : nullptr;
}

// ---- Client side ----

std::size_t ProtocolManager::GetPackedSize(KvOpcode opcode, const KvRequest& req) const
{
    KvProtocol* proto = GetProtocol(opcode);
    if (!proto) { return 0; }
    return proto->PackedSize(req);
}

std::size_t ProtocolManager::GetPackedResponseSize(KvOpcode opcode,
                                                   std::size_t result_count) const
{
    auto* proto = GetProtocol(opcode);
    return proto ? proto->PackedResponseSize(result_count) : 0;
}

Status ProtocolManager::PackRequest(void* data, KvOpcode opcode, const KvRequest& req)
{
    KvProtocol* proto = GetProtocol(opcode);
    if (!proto) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "unknown opcode in PackRequest");
    }
    return proto->PackRequest(req, data);
}

Status ProtocolManager::UnpackResponse(const void* data, KvOpcode opcode,
                                       std::uint16_t result_count, KvResponse& out)
{
    KvProtocol* proto = GetProtocol(opcode);
    if (!proto) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "unknown opcode in UnpackResponse");
    }
    return proto->UnpackResponse(data, result_count, out);
}

// ---- Server side ----

Status ProtocolManager::UnpackRequest(const void* data, std::size_t size,
                                      std::unique_ptr<KvRequest>& out)
{
    if (!data || size < kKvHeaderSize) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "UnpackRequest: invalid data or size smaller than header");
    }
    KvOpcode opcode = PeekOpcode(data);
    KvProtocol* proto = GetProtocol(opcode);
    if (!proto) {
        return Status::Error(
            StatusCode::INVALID_ARGUMENT,
            "UnpackRequest: unknown opcode " + std::to_string(static_cast<std::uint8_t>(opcode)));
    }
    return proto->UnpackRequest(data, size, out);
}

Status ProtocolManager::PackResponse(void* data, KvOpcode opcode, const KvResponse& resp)
{
    KvProtocol* proto = GetProtocol(opcode);
    if (!proto) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "unknown opcode in PackResponse");
    }
    return proto->PackResponse(data, resp);
}

}  // namespace UC::DRAMPOOL
