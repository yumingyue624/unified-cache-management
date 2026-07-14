/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#include "kv_control_protocol.h"

#include <cstdint>
#include <cstring>
#include <limits>

namespace UC::DRAMPOOL {
namespace {

constexpr std::uint32_t kKvRequestEnvelopeMagic = 0x4B565231U;  // KVR1
constexpr std::uint16_t kKvRequestEnvelopeVersion = 1;
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = kMagicOffset + sizeof(std::uint32_t);
constexpr std::size_t kManagerIdSizeOffset = kVersionOffset + sizeof(std::uint16_t);
constexpr std::size_t kRequestSizeOffset = kManagerIdSizeOffset + sizeof(std::uint16_t);
constexpr std::size_t kKvRequestEnvelopeHeaderSize =
    kRequestSizeOffset + sizeof(std::uint32_t);

template <typename T>
void WriteField(std::uint8_t* target, std::size_t offset, T value)
{
    std::memcpy(target + offset, &value, sizeof(value));
}

template <typename T>
T ReadField(const std::uint8_t* source, std::size_t offset)
{
    T value{};
    std::memcpy(&value, source + offset, sizeof(value));
    return value;
}

}  // namespace

UC::Status PackKvRequestEnvelope(const transport::ManagerID& peerManagerId,
                                 const void* requestData, std::size_t requestSize,
                                 transport::Metadata& output)
{
    if (peerManagerId.empty() || requestData == nullptr || requestSize == 0) {
        return UC::Status::InvalidParam("KV request envelope is incomplete");
    }
    if (peerManagerId.size() > std::numeric_limits<std::uint16_t>::max() ||
        requestSize > std::numeric_limits<std::uint32_t>::max() ||
        requestSize > std::numeric_limits<std::size_t>::max() -
                          kKvRequestEnvelopeHeaderSize - peerManagerId.size()) {
        return UC::Status::InvalidParam("KV request envelope is too large");
    }

    const auto managerIdSize = static_cast<std::uint16_t>(peerManagerId.size());
    const auto encodedRequestSize = static_cast<std::uint32_t>(requestSize);
    output.resize(kKvRequestEnvelopeHeaderSize + peerManagerId.size() + requestSize);
    auto* bytes = output.data();
    WriteField(bytes, kMagicOffset, kKvRequestEnvelopeMagic);
    WriteField(bytes, kVersionOffset, kKvRequestEnvelopeVersion);
    WriteField(bytes, kManagerIdSizeOffset, managerIdSize);
    WriteField(bytes, kRequestSizeOffset, encodedRequestSize);
    std::memcpy(bytes + kKvRequestEnvelopeHeaderSize, peerManagerId.data(), peerManagerId.size());
    std::memcpy(bytes + kKvRequestEnvelopeHeaderSize + peerManagerId.size(), requestData,
                requestSize);
    return UC::Status::OK();
}

UC::Status UnpackKvRequestEnvelope(const transport::Metadata& input,
                                   KvRequestEnvelopeView& output)
{
    output = {};
    if (input.size() < kKvRequestEnvelopeHeaderSize) {
        return UC::Status::InvalidParam("KV request envelope is smaller than its header");
    }

    const auto* bytes = input.data();
    if (ReadField<std::uint32_t>(bytes, kMagicOffset) != kKvRequestEnvelopeMagic ||
        ReadField<std::uint16_t>(bytes, kVersionOffset) != kKvRequestEnvelopeVersion) {
        return UC::Status::InvalidParam("KV request envelope has an unsupported format");
    }

    const auto managerIdSize = ReadField<std::uint16_t>(bytes, kManagerIdSizeOffset);
    const auto requestSize = ReadField<std::uint32_t>(bytes, kRequestSizeOffset);
    if (managerIdSize == 0 || requestSize == 0) {
        return UC::Status::InvalidParam("KV request envelope has an empty field");
    }

    const auto encodedSize = static_cast<std::uint64_t>(kKvRequestEnvelopeHeaderSize) +
                             managerIdSize + requestSize;
    if (encodedSize != input.size()) {
        return UC::Status::InvalidParam("KV request envelope size does not match its header");
    }

    const auto* managerId = reinterpret_cast<const char*>(bytes + kKvRequestEnvelopeHeaderSize);
    output.peer_manager_id.assign(managerId, managerIdSize);
    output.request_data = managerId + managerIdSize;
    output.request_size = requestSize;
    return UC::Status::OK();
}

}  // namespace UC::DRAMPOOL
