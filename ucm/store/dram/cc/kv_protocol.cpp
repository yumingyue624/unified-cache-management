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
#include <type_traits>

namespace UC::DramPool {
namespace {

constexpr std::size_t kBitsPerByte = 8U;
constexpr std::size_t kLookupResultBitWidth = 1U;
constexpr std::size_t kDumpLoadResultBitWidth = 4U;

template <typename>
inline constexpr bool kUnsupportedRequest = false;

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

template <typename RequestT>
constexpr KvOpcode RequestOpcode()
{
    if constexpr (std::is_same_v<RequestT, KvDumpRequest>) {
        return KvOpcode::Dump;
    } else if constexpr (std::is_same_v<RequestT, KvLoadRequest>) {
        return KvOpcode::Load;
    } else if constexpr (std::is_same_v<RequestT, KvLookupRequest>) {
        return KvOpcode::Lookup;
    } else {
        static_assert(kUnsupportedRequest<RequestT>, "unsupported DramPool request type");
    }
}

template <typename RequestT>
constexpr std::size_t RequestHeaderSize()
{
    if constexpr (std::is_same_v<RequestT, KvDumpRequest>) {
        return kKvDumpRequestHeaderSize;
    } else if constexpr (std::is_same_v<RequestT, KvLoadRequest>) {
        return kKvLoadRequestHeaderSize;
    } else if constexpr (std::is_same_v<RequestT, KvLookupRequest>) {
        return kKvLookupRequestHeaderSize;
    } else {
        static_assert(kUnsupportedRequest<RequestT>, "unsupported DramPool request type");
    }
}

template <typename RequestT>
constexpr std::size_t RequestEntrySize()
{
    if constexpr (std::is_same_v<RequestT, KvDumpRequest>) {
        return kKvDumpEntrySize;
    } else if constexpr (std::is_same_v<RequestT, KvLoadRequest>) {
        return kKvLoadEntrySize;
    } else if constexpr (std::is_same_v<RequestT, KvLookupRequest>) {
        return kKvLookupEntrySize;
    } else {
        static_assert(kUnsupportedRequest<RequestT>, "unsupported DramPool request type");
    }
}

template <typename RequestT>
constexpr std::size_t RequestBatchSizeOffset()
{
    if constexpr (std::is_same_v<RequestT, KvDumpRequest>) {
        return kDumpBatchSizeOffset;
    } else if constexpr (std::is_same_v<RequestT, KvLoadRequest> ||
                         std::is_same_v<RequestT, KvLookupRequest>) {
        return kLoadLookupBatchSizeOffset;
    } else {
        static_assert(kUnsupportedRequest<RequestT>, "unsupported DramPool request type");
    }
}

template <typename EntryT>
void PackDumpLoadEntry(std::uint8_t* output, const EntryT& entry)
{
    std::memcpy(output + kDumpLoadEntryKeyOffset, entry.key.data(), kKvKeySize);
    std::memcpy(output + kDumpLoadEntryAddrOffset, &entry.addr, sizeof(entry.addr));
    std::memcpy(output + kDumpLoadEntryLenOffset, &entry.len, sizeof(entry.len));
    std::memcpy(output + kDumpLoadEntryIdxOffset, &entry.idx, sizeof(entry.idx));
}

template <typename EntryT>
Status ValidateDumpLoadEntry(const EntryT& entry, const char* protocol, std::size_t index)
{
    const std::string prefix = std::string{protocol} + ": entry[" + std::to_string(index) + "] ";
    if (IsAllZeroKey(entry.key)) { return Status::InvalidParam(prefix + "key is all zero"); }
    if (entry.addr == 0) { return Status::InvalidParam(prefix + "addr is zero"); }
    if (entry.len == 0) { return Status::InvalidParam(prefix + "len is zero"); }
    return Status::OK();
}

template <typename EntryT>
Status UnpackDumpLoadEntry(const std::uint8_t* input, EntryT& entry, const char* protocol,
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

template <typename RequestT>
Status ValidateRequest(const RequestT& request)
{
    constexpr auto opcode = RequestOpcode<RequestT>();
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

template <typename RequestT>
void PackCommonRequestHeader(std::uint8_t* output, const RequestT& request)
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

template <typename RequestT>
std::size_t GetPackedRequestSize(const RequestT& request)
{
    return RequestHeaderSize<RequestT>() +
           static_cast<std::size_t>(request.batch_size) * RequestEntrySize<RequestT>();
}

template <typename RequestT>
Status PackRequestData(const RequestT& request, void* target)
{
    const std::string protocol = ProtocolName(RequestOpcode<RequestT>());
    if (!target) { return Status::InvalidParam(protocol + ": PackRequest target is null"); }
    if (auto status = ValidateRequest(request); status.Failure()) { return status; }

    auto* output = static_cast<std::uint8_t*>(target);
    PackRequestHeader(output, request);
    for (std::size_t index = 0; index < request.entries.size(); ++index) {
        PackEntry(output + RequestHeaderSize<RequestT>() + index * RequestEntrySize<RequestT>(),
                  request.entries[index]);
    }
    return Status::OK();
}

template <typename RequestT>
Status UnpackRequestData(const void* data, std::size_t size, RequestT& request)
{
    constexpr auto opcode = RequestOpcode<RequestT>();
    const std::string protocol = ProtocolName(opcode);
    if (!data || size < RequestHeaderSize<RequestT>()) {
        return Status::InvalidParam(protocol + ": invalid data or size smaller than header");
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    RequestT decoded;
    decoded.opcode = PeekOpcode(data);
    if (decoded.opcode != opcode) { return Status::InvalidParam(protocol + ": opcode mismatch"); }
    std::memcpy(&decoded.resp_addr, bytes + kRespAddrOffset, sizeof(decoded.resp_addr));
    if (decoded.resp_addr == 0) { return Status::InvalidParam(protocol + ": resp_addr is zero"); }
    UnpackExtraHeader(bytes, decoded);
    std::memcpy(&decoded.batch_size, bytes + RequestBatchSizeOffset<RequestT>(),
                sizeof(decoded.batch_size));
    if (decoded.batch_size == 0) { return Status::InvalidParam(protocol + ": batch_size is zero"); }

    const std::size_t expectedSize =
        RequestHeaderSize<RequestT>() +
        static_cast<std::size_t>(decoded.batch_size) * RequestEntrySize<RequestT>();
    if (size != expectedSize) {
        return Status::InvalidParam(protocol + ": size(" + std::to_string(size) + ") != expected(" +
                                    std::to_string(expectedSize) + ")");
    }

    decoded.entries.resize(decoded.batch_size);
    for (std::size_t index = 0; index < decoded.entries.size(); ++index) {
        if (auto status = UnpackEntry(
                bytes + RequestHeaderSize<RequestT>() + index * RequestEntrySize<RequestT>(),
                decoded.entries[index], index);
            status.Failure()) {
            return status;
        }
    }
    request = std::move(decoded);
    return Status::OK();
}

constexpr std::size_t GetPackedResultSize(std::size_t resultCount, std::size_t resultBitWidth)
{
    const std::size_t resultsPerByte = kBitsPerByte / resultBitWidth;
    return resultCount / resultsPerByte + (resultCount % resultsPerByte != 0U ? 1U : 0U);
}

Status PackResults(void* data, const std::vector<std::uint8_t>& results, std::size_t resultBitWidth,
                   const char* protocol)
{
    const std::size_t resultsPerByte = kBitsPerByte / resultBitWidth;
    const auto maxResult = static_cast<std::uint8_t>((1U << resultBitWidth) - 1U);
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (results[index] > maxResult) {
            return Status::InvalidParam(std::string{protocol} + ": result[" +
                                        std::to_string(index) + "] exceeds " +
                                        std::to_string(resultBitWidth) + " bits");
        }
    }

    auto* packed = static_cast<std::uint8_t*>(data);
    std::fill_n(packed, GetPackedResultSize(results.size(), resultBitWidth), std::uint8_t{0});
    for (std::size_t index = 0; index < results.size(); ++index) {
        const std::size_t bitOffset = (index % resultsPerByte) * resultBitWidth;
        packed[index / resultsPerByte] = static_cast<std::uint8_t>(packed[index / resultsPerByte] |
                                                                   (results[index] << bitOffset));
    }
    return Status::OK();
}

void UnpackResults(const void* data, std::size_t resultCount, std::size_t resultBitWidth,
                   std::vector<std::uint8_t>& results)
{
    const auto* packed = static_cast<const std::uint8_t*>(data);
    const std::size_t resultsPerByte = kBitsPerByte / resultBitWidth;
    const auto resultMask = static_cast<std::uint8_t>((1U << resultBitWidth) - 1U);
    results.resize(resultCount);
    for (std::size_t index = 0; index < resultCount; ++index) {
        const std::size_t bitOffset = (index % resultsPerByte) * resultBitWidth;
        results[index] =
            static_cast<std::uint8_t>((packed[index / resultsPerByte] >> bitOffset) & resultMask);
    }
}

std::size_t GetPackedDumpLoadResponseSize(std::size_t resultCount)
{
    return kResponseResultsOffset + GetPackedResultSize(resultCount, kDumpLoadResultBitWidth);
}

std::size_t GetPackedLookupResponseSize(std::size_t resultCount)
{
    return kResponseResultsOffset + GetPackedResultSize(resultCount, kLookupResultBitWidth);
}

Status IsResponseReady(const void* data, bool& ready)
{
    ready = false;
    if (!data) { return Status::InvalidParam("IsResponseReady data is null"); }
    const auto* bytes = static_cast<const volatile std::uint8_t*>(data);
    switch (static_cast<ResponseStatus>(bytes[kResponseStatusOffset])) {
        case ResponseStatus::Pending: return Status::OK();
        case ResponseStatus::Ready: ready = true; return Status::OK();
        default: return Status::InvalidParam("unknown response status");
    }
}

Status PackResponseData(void* data, const KvResponse& response, std::size_t resultBitWidth,
                        bool allowEmpty, const char* protocol)
{
    if (!data) {
        return Status::InvalidParam(std::string{protocol} + ": PackResponse data is null");
    }
    if (!allowEmpty && response.results.empty()) {
        return Status::InvalidParam(std::string{protocol} + ": results must not be empty");
    }

    auto* bytes = static_cast<std::uint8_t*>(data);
    if (auto status =
            PackResults(bytes + kResponseResultsOffset, response.results, resultBitWidth, protocol);
        status.Failure()) {
        return status;
    }
    bytes[kResponseStatusOffset] = static_cast<std::uint8_t>(ResponseStatus::Ready);
    return Status::OK();
}

Status UnpackResponseData(const void* data, std::uint16_t resultCount, std::size_t resultBitWidth,
                          const char* protocol, KvResponse& out)
{
    if (!data) {
        return Status::InvalidParam(std::string{protocol} + ": UnpackResponse data is null");
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    UnpackResults(bytes + kResponseResultsOffset, resultCount, resultBitWidth, out.results);
    return Status::OK();
}

template <typename RequestT>
Status UnpackTypedRequest(const void* data, std::size_t size, std::unique_ptr<KvRequest>& out)
{
    auto request = std::make_unique<RequestT>();
    auto status = request->UnpackRequest(data, size);
    if (status.Success()) { out = std::move(request); }
    return status;
}

}  // namespace

std::size_t KvDumpRequest::GetPackedRequestSize() const
{
    return UC::DramPool::GetPackedRequestSize(*this);
}

Status KvDumpRequest::PackRequest(void* target) const { return PackRequestData(*this, target); }

Status KvDumpRequest::UnpackRequest(const void* data, std::size_t size)
{
    return UnpackRequestData(data, size, *this);
}

std::size_t KvDumpRequest::GetPackedResponseSize(std::size_t result_count)
{
    return GetPackedDumpLoadResponseSize(result_count);
}

Status KvDumpRequest::IsResponseReady(const void* data, bool& ready)
{
    return UC::DramPool::IsResponseReady(data, ready);
}

Status KvDumpRequest::PackResponse(void* data, const KvResponse& response)
{
    return PackResponseData(data, response, kDumpLoadResultBitWidth, true, "Dump");
}

Status KvDumpRequest::UnpackResponse(const void* data, std::uint16_t result_count, KvResponse& out)
{
    return UnpackResponseData(data, result_count, kDumpLoadResultBitWidth, "Dump", out);
}

std::size_t KvLoadRequest::GetPackedRequestSize() const
{
    return UC::DramPool::GetPackedRequestSize(*this);
}

Status KvLoadRequest::PackRequest(void* target) const { return PackRequestData(*this, target); }

Status KvLoadRequest::UnpackRequest(const void* data, std::size_t size)
{
    return UnpackRequestData(data, size, *this);
}

std::size_t KvLoadRequest::GetPackedResponseSize(std::size_t result_count)
{
    return GetPackedDumpLoadResponseSize(result_count);
}

Status KvLoadRequest::IsResponseReady(const void* data, bool& ready)
{
    return UC::DramPool::IsResponseReady(data, ready);
}

Status KvLoadRequest::PackResponse(void* data, const KvResponse& response)
{
    return PackResponseData(data, response, kDumpLoadResultBitWidth, true, "Load");
}

Status KvLoadRequest::UnpackResponse(const void* data, std::uint16_t result_count, KvResponse& out)
{
    return UnpackResponseData(data, result_count, kDumpLoadResultBitWidth, "Load", out);
}

std::size_t KvLookupRequest::GetPackedRequestSize() const
{
    return UC::DramPool::GetPackedRequestSize(*this);
}

Status KvLookupRequest::PackRequest(void* target) const { return PackRequestData(*this, target); }

Status KvLookupRequest::UnpackRequest(const void* data, std::size_t size)
{
    return UnpackRequestData(data, size, *this);
}

std::size_t KvLookupRequest::GetPackedResponseSize(std::size_t result_count)
{
    return GetPackedLookupResponseSize(result_count);
}

Status KvLookupRequest::IsResponseReady(const void* data, bool& ready)
{
    return UC::DramPool::IsResponseReady(data, ready);
}

Status KvLookupRequest::PackResponse(void* data, const KvResponse& response)
{
    return PackResponseData(data, response, kLookupResultBitWidth, false, "Lookup");
}

Status KvLookupRequest::UnpackResponse(const void* data, std::uint16_t result_count,
                                       KvResponse& out)
{
    return UnpackResponseData(data, result_count, kLookupResultBitWidth, "Lookup", out);
}

template <typename RequestT>
std::size_t KvProtocol<RequestT>::GetPackedRequestSize(const RequestT& request) const
{
    return request.GetPackedRequestSize();
}

template <typename RequestT>
Status KvProtocol<RequestT>::PackRequest(const RequestT& request, void* target) const
{
    return request.PackRequest(target);
}

template <typename RequestT>
Status KvProtocol<RequestT>::UnpackRequest(const void* data, std::size_t size,
                                           std::unique_ptr<RequestT>& out) const
{
    auto request = std::make_unique<RequestT>();
    auto status = request->UnpackRequest(data, size);
    if (status.Success()) { out = std::move(request); }
    return status;
}

template <typename RequestT>
std::size_t KvProtocol<RequestT>::GetPackedResponseSize(std::size_t result_count) const
{
    return RequestT::GetPackedResponseSize(result_count);
}

template <typename RequestT>
Status KvProtocol<RequestT>::IsResponseReady(const void* data, bool& ready) const
{
    return RequestT::IsResponseReady(data, ready);
}

template <typename RequestT>
Status KvProtocol<RequestT>::PackResponse(void* data, const KvResponse& response) const
{
    return RequestT::PackResponse(data, response);
}

template <typename RequestT>
Status KvProtocol<RequestT>::UnpackResponse(const void* data, std::uint16_t result_count,
                                            KvResponse& out) const
{
    return RequestT::UnpackResponse(data, result_count, out);
}

template class KvProtocol<KvDumpRequest>;
template class KvProtocol<KvLoadRequest>;
template class KvProtocol<KvLookupRequest>;

template <typename RequestT>
std::size_t ProtocolManager::GetPackedRequestSize(const RequestT& request) const
{
    return KvProtocol<RequestT>{}.GetPackedRequestSize(request);
}

template <typename RequestT>
Status ProtocolManager::PackRequest(void* data, const RequestT& request) const
{
    return KvProtocol<RequestT>{}.PackRequest(request, data);
}

template std::size_t ProtocolManager::GetPackedRequestSize(const KvDumpRequest&) const;
template std::size_t ProtocolManager::GetPackedRequestSize(const KvLoadRequest&) const;
template std::size_t ProtocolManager::GetPackedRequestSize(const KvLookupRequest&) const;
template Status ProtocolManager::PackRequest(void*, const KvDumpRequest&) const;
template Status ProtocolManager::PackRequest(void*, const KvLoadRequest&) const;
template Status ProtocolManager::PackRequest(void*, const KvLookupRequest&) const;

std::size_t ProtocolManager::GetPackedResponseSize(KvOpcode opcode, std::size_t result_count) const
{
    switch (opcode) {
        case KvOpcode::Dump: return KvDumpRequest::GetPackedResponseSize(result_count);
        case KvOpcode::Load: return KvLoadRequest::GetPackedResponseSize(result_count);
        case KvOpcode::Lookup: return KvLookupRequest::GetPackedResponseSize(result_count);
        default: return 0;
    }
}

Status ProtocolManager::IsResponseReady(const void* data, bool& ready) const
{
    return KvDumpRequest::IsResponseReady(data, ready);
}

Status ProtocolManager::UnpackResponse(const void* data, KvOpcode opcode,
                                       std::uint16_t result_count, KvResponse& out)
{
    bool ready = false;
    if (auto status = IsResponseReady(data, ready); status.Failure()) { return status; }
    if (!ready) { return Status::Retry(); }
    switch (opcode) {
        case KvOpcode::Dump: return KvDumpRequest::UnpackResponse(data, result_count, out);
        case KvOpcode::Load: return KvLoadRequest::UnpackResponse(data, result_count, out);
        case KvOpcode::Lookup: return KvLookupRequest::UnpackResponse(data, result_count, out);
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

Status ProtocolManager::PackResponse(void* data, KvOpcode opcode, const KvResponse& response)
{
    switch (opcode) {
        case KvOpcode::Dump: return KvDumpRequest::PackResponse(data, response);
        case KvOpcode::Load: return KvLoadRequest::PackResponse(data, response);
        case KvOpcode::Lookup: return KvLookupRequest::PackResponse(data, response);
        default: return Status::InvalidParam("unknown opcode in PackResponse");
    }
}

}  // namespace UC::DramPool
