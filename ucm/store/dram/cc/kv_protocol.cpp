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

namespace UC::DramPool {
namespace {

bool IsAllZeroKey(const BlockId& key)
{
    return std::all_of(key.begin(), key.end(),
                       [](std::byte value) { return value == std::byte{}; });
}

KvOpcode PeekOpcode(const void* data)
{
    return static_cast<KvOpcode>(static_cast<const std::uint8_t*>(data)[kOpcodeOffset]);
}

const char* ProtocolName(KvOpcode opcode)
{
    switch (opcode) {
        case KvOpcode::Dump: return "Dump";
        case KvOpcode::Load: return "Load";
        case KvOpcode::Lookup: return "Lookup";
        default: return "Unknown";
    }
}

constexpr KvOpcode RequestOpcode(const KvDumpRequest&) { return KvOpcode::Dump; }
constexpr KvOpcode RequestOpcode(const KvLoadRequest&) { return KvOpcode::Load; }
constexpr KvOpcode RequestOpcode(const KvLookupRequest&) { return KvOpcode::Lookup; }

constexpr std::size_t RequestHeaderSize(const KvDumpRequest&) { return kKvDumpRequestHeaderSize; }

constexpr std::size_t RequestHeaderSize(const KvLoadRequest&) { return kKvLoadRequestHeaderSize; }

constexpr std::size_t RequestHeaderSize(const KvLookupRequest&)
{
    return kKvLookupRequestHeaderSize;
}

constexpr std::size_t RequestEntrySize(const KvDumpRequest&) { return kKvDumpEntrySize; }
constexpr std::size_t RequestEntrySize(const KvLoadRequest&) { return kKvLoadEntrySize; }
constexpr std::size_t RequestEntrySize(const KvLookupRequest&) { return kKvLookupEntrySize; }

constexpr std::size_t BatchSizeOffset(const KvDumpRequest&) { return kDumpBatchSizeOffset; }

constexpr std::size_t BatchSizeOffset(const KvLoadRequest&) { return kLoadLookupBatchSizeOffset; }

constexpr std::size_t BatchSizeOffset(const KvLookupRequest&) { return kLoadLookupBatchSizeOffset; }

template <typename Entry>
void PackDumpLoadEntry(std::uint8_t* output, const Entry& entry)
{
    std::memcpy(output + kDumpLoadEntryKeyOffset, entry.key.data(), kKvKeySize);
    std::memcpy(output + kDumpLoadEntryAddrOffset, &entry.addr, sizeof(entry.addr));
    std::memcpy(output + kDumpLoadEntryLenOffset, &entry.len, sizeof(entry.len));
    std::memcpy(output + kDumpLoadEntryIdxOffset, &entry.idx, sizeof(entry.idx));
}

template <typename Entry>
Status ValidateDumpLoadEntry(const Entry& entry, const char* protocol, std::size_t index)
{
    if (IsAllZeroKey(entry.key)) {
        return Status::InvalidParam(std::string{protocol} + ": entry[" + std::to_string(index) +
                                    "] key is all zero");
    }
    if (entry.addr == 0) {
        return Status::InvalidParam(std::string{protocol} + ": entry[" + std::to_string(index) +
                                    "] addr is zero");
    }
    if (entry.len == 0) {
        return Status::InvalidParam(std::string{protocol} + ": entry[" + std::to_string(index) +
                                    "] len is zero");
    }
    return Status::OK();
}

template <typename Entry>
Status UnpackDumpLoadEntry(const std::uint8_t* input, Entry& entry, const char* protocol,
                           std::size_t index)
{
    std::memcpy(entry.key.data(), input + kDumpLoadEntryKeyOffset, kKvKeySize);
    std::memcpy(&entry.addr, input + kDumpLoadEntryAddrOffset, sizeof(entry.addr));
    std::memcpy(&entry.len, input + kDumpLoadEntryLenOffset, sizeof(entry.len));
    std::memcpy(&entry.idx, input + kDumpLoadEntryIdxOffset, sizeof(entry.idx));
    return ValidateDumpLoadEntry(entry, protocol, index);
}

Status ValidateEntry(const KvDumpEntry& entry, std::size_t index)
{
    return ValidateDumpLoadEntry(entry, "Dump", index);
}

Status ValidateEntry(const KvLoadEntry& entry, std::size_t index)
{
    return ValidateDumpLoadEntry(entry, "Load", index);
}

Status ValidateEntry(const KvLookupEntry& entry, std::size_t index)
{
    if (IsAllZeroKey(entry.key)) {
        return Status::InvalidParam("Lookup: entry[" + std::to_string(index) + "] key is all zero");
    }
    return Status::OK();
}

void PackEntry(std::uint8_t* output, const KvDumpEntry& entry) { PackDumpLoadEntry(output, entry); }

void PackEntry(std::uint8_t* output, const KvLoadEntry& entry) { PackDumpLoadEntry(output, entry); }

void PackEntry(std::uint8_t* output, const KvLookupEntry& entry)
{
    std::memcpy(output + kLookupEntryKeyOffset, entry.key.data(), kKvKeySize);
}

Status UnpackEntry(const std::uint8_t* input, KvDumpEntry& entry, std::size_t index)
{
    return UnpackDumpLoadEntry(input, entry, "Dump", index);
}

Status UnpackEntry(const std::uint8_t* input, KvLoadEntry& entry, std::size_t index)
{
    return UnpackDumpLoadEntry(input, entry, "Load", index);
}

Status UnpackEntry(const std::uint8_t* input, KvLookupEntry& entry, std::size_t index)
{
    std::memcpy(entry.key.data(), input + kLookupEntryKeyOffset, kKvKeySize);
    return ValidateEntry(entry, index);
}

template <typename Request>
Status ValidateRequest(const Request& request)
{
    const auto opcode = RequestOpcode(request);
    const std::string protocol = ProtocolName(opcode);
    if (request.opcode != opcode) { return Status::InvalidParam(protocol + ": opcode mismatch"); }
    if (request.resp_addr == 0) { return Status::InvalidParam(protocol + ": resp_addr is zero"); }
    if (request.batch_size == 0 || request.batch_size != request.entries.size()) {
        return Status::InvalidParam(protocol + ": batch_size(" +
                                    std::to_string(request.batch_size) +
                                    ") must be non-zero and equal to entries.size()(" +
                                    std::to_string(request.entries.size()) + ")");
    }
    for (std::size_t index = 0; index < request.entries.size(); ++index) {
        if (auto status = ValidateEntry(request.entries[index], index); status.Failure()) {
            return status;
        }
    }
    return Status::OK();
}

void PackRequestHeader(std::uint8_t* output, const KvDumpRequest& request)
{
    output[kOpcodeOffset] = static_cast<std::uint8_t>(request.opcode);
    std::memcpy(output + kRespAddrOffset, &request.resp_addr, sizeof(request.resp_addr));
    std::memcpy(output + kDumpTtlOffset, &request.ttl, sizeof(request.ttl));
    std::memcpy(output + kDumpBatchSizeOffset, &request.batch_size, sizeof(request.batch_size));
}

template <typename Request>
void PackCommonRequestHeader(std::uint8_t* output, const Request& request)
{
    output[kOpcodeOffset] = static_cast<std::uint8_t>(request.opcode);
    std::memcpy(output + kRespAddrOffset, &request.resp_addr, sizeof(request.resp_addr));
    std::memcpy(output + kLoadLookupBatchSizeOffset, &request.batch_size,
                sizeof(request.batch_size));
}

void PackRequestHeader(std::uint8_t* output, const KvLoadRequest& request)
{
    PackCommonRequestHeader(output, request);
}

void PackRequestHeader(std::uint8_t* output, const KvLookupRequest& request)
{
    PackCommonRequestHeader(output, request);
}

void UnpackExtraHeader(const std::uint8_t* input, KvDumpRequest& request)
{
    std::memcpy(&request.ttl, input + kDumpTtlOffset, sizeof(request.ttl));
}

void UnpackExtraHeader(const std::uint8_t*, KvLoadRequest&) {}
void UnpackExtraHeader(const std::uint8_t*, KvLookupRequest&) {}

struct FourBitResultCodec {
    static constexpr bool kAllowEmpty = true;

    static std::size_t PackedSize(std::size_t resultCount)
    {
        return Packed4BitResultSize(resultCount);
    }

    static Status Pack(void* data, const std::vector<std::uint8_t>& results,
                       const std::string& protocol)
    {
        for (std::size_t index = 0; index < results.size(); ++index) {
            if (results[index] > 0x0FU) {
                return Status::InvalidParam(protocol + ": result[" + std::to_string(index) +
                                            "] exceeds 4 bits");
            }
        }
        auto* packed = static_cast<std::uint8_t*>(data);
        for (std::size_t byteIndex = 0; byteIndex < PackedSize(results.size()); ++byteIndex) {
            const std::size_t first = byteIndex * 2U;
            std::uint8_t value = results[first];
            if (first + 1U < results.size()) {
                value = static_cast<std::uint8_t>(value | (results[first + 1U] << 4U));
            }
            packed[byteIndex] = value;
        }
        return Status::OK();
    }

    static void Unpack(const void* data, std::size_t resultCount,
                       std::vector<std::uint8_t>& results)
    {
        const auto* packed = static_cast<const std::uint8_t*>(data);
        results.resize(resultCount);
        for (std::size_t index = 0; index < resultCount; ++index) {
            results[index] =
                static_cast<std::uint8_t>((packed[index / 2U] >> ((index % 2U) * 4U)) & 0x0FU);
        }
    }
};

struct OneBitResultCodec {
    static constexpr bool kAllowEmpty = false;

    static std::size_t PackedSize(std::size_t resultCount)
    {
        return Packed1BitResultSize(resultCount);
    }

    static Status Pack(void* data, const std::vector<std::uint8_t>& results,
                       const std::string& protocol)
    {
        for (std::size_t index = 0; index < results.size(); ++index) {
            if (results[index] > 1U) {
                return Status::InvalidParam(protocol + ": result[" + std::to_string(index) +
                                            "] exceeds 1 bit");
            }
        }
        auto* packed = static_cast<std::uint8_t*>(data);
        for (std::size_t byteIndex = 0; byteIndex < PackedSize(results.size()); ++byteIndex) {
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

    static void Unpack(const void* data, std::size_t resultCount,
                       std::vector<std::uint8_t>& results)
    {
        const auto* packed = static_cast<const std::uint8_t*>(data);
        results.resize(resultCount);
        for (std::size_t index = 0; index < resultCount; ++index) {
            results[index] =
                static_cast<std::uint8_t>((packed[index / 8U] >> (index % 8U)) & 0x01U);
        }
    }
};

template <typename RequestT>
struct ResultCodec;

template <>
struct ResultCodec<KvDumpRequest> : FourBitResultCodec {};

template <>
struct ResultCodec<KvLoadRequest> : FourBitResultCodec {};

template <>
struct ResultCodec<KvLookupRequest> : OneBitResultCodec {};

template <typename Request>
Status UnpackTypedRequest(const void* data, std::size_t size, std::unique_ptr<KvRequest>& out)
{
    std::unique_ptr<Request> request;
    auto status = KvProtocol<Request>{}.UnpackRequest(data, size, request);
    if (status.Success()) { out = std::move(request); }
    return status;
}

}  // namespace

template <typename RequestT>
std::size_t KvProtocol<RequestT>::PackedSize(const RequestT& req) const
{
    return RequestHeaderSize(req) +
           static_cast<std::size_t>(req.batch_size) * RequestEntrySize(req);
}

template <typename RequestT>
std::size_t KvProtocol<RequestT>::PackedResponseSize(std::size_t result_count) const
{
    return kResponseResultsOffset + ResultCodec<RequestT>::PackedSize(result_count);
}

template <typename RequestT>
Status KvProtocol<RequestT>::PackRequest(const RequestT& req, void* target) const
{
    if (!target) {
        return Status::InvalidParam(std::string{ProtocolName(RequestOpcode(req))} +
                                    ": PackRequest target is null");
    }
    if (auto status = ValidateRequest(req); status.Failure()) { return status; }

    auto* output = static_cast<std::uint8_t*>(target);
    PackRequestHeader(output, req);
    for (std::size_t index = 0; index < req.entries.size(); ++index) {
        PackEntry(output + RequestHeaderSize(req) + index * RequestEntrySize(req),
                  req.entries[index]);
    }
    return Status::OK();
}

template <typename RequestT>
Status KvProtocol<RequestT>::UnpackResponse(const void* data, std::uint16_t result_count,
                                            KvResponse& out) const
{
    if (!data) {
        return Status::InvalidParam(std::string{ProtocolName(RequestOpcode(RequestT{}))} +
                                    ": UnpackResponse data is null");
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    ResultCodec<RequestT>::Unpack(bytes + kResponseResultsOffset, result_count, out.results);
    return Status::OK();
}

template <typename RequestT>
Status KvProtocol<RequestT>::UnpackRequest(const void* data, std::size_t size,
                                           std::unique_ptr<RequestT>& out) const
{
    const RequestT requestTag;
    const auto opcode = RequestOpcode(requestTag);
    const std::string protocol = ProtocolName(opcode);
    if (!data || size < RequestHeaderSize(requestTag)) {
        return Status::InvalidParam(protocol + ": invalid data or size smaller than header");
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    auto request = std::make_unique<RequestT>();
    request->opcode = PeekOpcode(data);
    if (request->opcode != opcode) { return Status::InvalidParam(protocol + ": opcode mismatch"); }
    std::memcpy(&request->resp_addr, bytes + kRespAddrOffset, sizeof(request->resp_addr));
    if (request->resp_addr == 0) { return Status::InvalidParam(protocol + ": resp_addr is zero"); }
    UnpackExtraHeader(bytes, *request);
    std::memcpy(&request->batch_size, bytes + BatchSizeOffset(*request),
                sizeof(request->batch_size));
    if (request->batch_size == 0) {
        return Status::InvalidParam(protocol + ": batch_size is zero");
    }

    const std::size_t expectedSize =
        RequestHeaderSize(*request) +
        static_cast<std::size_t>(request->batch_size) * RequestEntrySize(*request);
    if (size != expectedSize) {
        return Status::InvalidParam(protocol + ": size(" + std::to_string(size) + ") != expected(" +
                                    std::to_string(expectedSize) + ")");
    }

    request->entries.resize(request->batch_size);
    for (std::size_t index = 0; index < request->entries.size(); ++index) {
        if (auto status = UnpackEntry(
                bytes + RequestHeaderSize(*request) + index * RequestEntrySize(*request),
                request->entries[index], index);
            status.Failure()) {
            return status;
        }
    }
    out = std::move(request);
    return Status::OK();
}

template <typename RequestT>
Status KvProtocol<RequestT>::PackResponse(void* data, const KvResponse& resp) const
{
    const RequestT request;
    const std::string protocol = ProtocolName(RequestOpcode(request));
    if (!data) { return Status::InvalidParam(protocol + ": PackResponse data is null"); }
    if (!ResultCodec<RequestT>::kAllowEmpty && resp.results.empty()) {
        return Status::InvalidParam(protocol + ": results must not be empty");
    }
    auto* bytes = static_cast<std::uint8_t*>(data);
    if (auto status =
            ResultCodec<RequestT>::Pack(bytes + kResponseResultsOffset, resp.results, protocol);
        status.Failure()) {
        return status;
    }
    bytes[kResponseStatusOffset] = static_cast<std::uint8_t>(ResponseStatus::Ready);
    return Status::OK();
}

template class KvProtocol<KvDumpRequest>;
template class KvProtocol<KvLoadRequest>;
template class KvProtocol<KvLookupRequest>;

template <typename RequestT>
std::size_t ProtocolManager::GetPackedSize(const RequestT& req) const
{
    return KvProtocol<RequestT>{}.PackedSize(req);
}

template <typename RequestT>
Status ProtocolManager::PackRequest(void* data, const RequestT& req) const
{
    return KvProtocol<RequestT>{}.PackRequest(req, data);
}

template std::size_t ProtocolManager::GetPackedSize(const KvDumpRequest&) const;
template std::size_t ProtocolManager::GetPackedSize(const KvLoadRequest&) const;
template std::size_t ProtocolManager::GetPackedSize(const KvLookupRequest&) const;
template Status ProtocolManager::PackRequest(void*, const KvDumpRequest&) const;
template Status ProtocolManager::PackRequest(void*, const KvLoadRequest&) const;
template Status ProtocolManager::PackRequest(void*, const KvLookupRequest&) const;

std::size_t ProtocolManager::GetPackedResponseSize(KvOpcode opcode, std::size_t result_count) const
{
    switch (opcode) {
        case KvOpcode::Dump: return KvDumpProtocol{}.PackedResponseSize(result_count);
        case KvOpcode::Load: return KvLoadProtocol{}.PackedResponseSize(result_count);
        case KvOpcode::Lookup: return KvLookupProtocol{}.PackedResponseSize(result_count);
        default: return 0;
    }
}

Status ProtocolManager::IsResponseReady(const void* data, bool& ready) const
{
    ready = false;
    if (!data) { return Status::InvalidParam("IsResponseReady data is null"); }
    const auto* bytes = static_cast<const volatile std::uint8_t*>(data);
    const auto status = static_cast<ResponseStatus>(bytes[kResponseStatusOffset]);
    switch (status) {
        case ResponseStatus::Pending: return Status::OK();
        case ResponseStatus::Ready: ready = true; return Status::OK();
        default: return Status::InvalidParam("unknown response status");
    }
}

Status ProtocolManager::UnpackResponse(const void* data, KvOpcode opcode,
                                       std::uint16_t result_count, KvResponse& out)
{
    bool ready = false;
    if (auto status = IsResponseReady(data, ready); status.Failure()) { return status; }
    if (!ready) { return Status::Retry(); }
    switch (opcode) {
        case KvOpcode::Dump: return KvDumpProtocol{}.UnpackResponse(data, result_count, out);
        case KvOpcode::Load: return KvLoadProtocol{}.UnpackResponse(data, result_count, out);
        case KvOpcode::Lookup: return KvLookupProtocol{}.UnpackResponse(data, result_count, out);
        default: return Status::InvalidParam("unknown opcode in UnpackResponse");
    }
}

Status ProtocolManager::UnpackRequest(const void* data, std::size_t size,
                                      std::unique_ptr<KvRequest>& out)
{
    if (!data || size < sizeof(std::uint8_t)) {
        return Status::InvalidParam("UnpackRequest: invalid data or missing opcode");
    }
    const auto opcode = PeekOpcode(data);
    switch (opcode) {
        case KvOpcode::Dump: return UnpackTypedRequest<KvDumpRequest>(data, size, out);
        case KvOpcode::Load: return UnpackTypedRequest<KvLoadRequest>(data, size, out);
        case KvOpcode::Lookup: return UnpackTypedRequest<KvLookupRequest>(data, size, out);
        default:
            return Status::InvalidParam("UnpackRequest: unknown opcode " +
                                        std::to_string(static_cast<std::uint8_t>(opcode)));
    }
}

Status ProtocolManager::PackResponse(void* data, KvOpcode opcode, const KvResponse& resp)
{
    switch (opcode) {
        case KvOpcode::Dump: return KvDumpProtocol{}.PackResponse(data, resp);
        case KvOpcode::Load: return KvLoadProtocol{}.PackResponse(data, resp);
        case KvOpcode::Lookup: return KvLookupProtocol{}.PackResponse(data, resp);
        default: return Status::InvalidParam("unknown opcode in PackResponse");
    }
}

}  // namespace UC::DramPool
