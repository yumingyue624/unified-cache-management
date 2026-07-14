/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#include <array>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include "kv_control_protocol.h"

namespace UC::DRAMPOOL {

TEST(KvControlProtocolTest, PreservesManagerIdentityOutsideKvPayload)
{
    constexpr char kPeerManagerId[] = "192.0.2.10:4502";
    const std::array<std::uint8_t, 5> request{{0x01, 0x00, 0xFF, 0x02, 0x03}};
    transport::Metadata encoded;

    ASSERT_TRUE(PackKvRequestEnvelope(kPeerManagerId, request.data(), request.size(), encoded)
                    .Success());

    KvRequestEnvelopeView decoded;
    ASSERT_TRUE(UnpackKvRequestEnvelope(encoded, decoded).Success());
    EXPECT_EQ(decoded.peer_manager_id, kPeerManagerId);
    ASSERT_EQ(decoded.request_size, request.size());
    EXPECT_EQ(std::memcmp(decoded.request_data, request.data(), request.size()), 0);
}

TEST(KvControlProtocolTest, RejectsTruncatedPayload)
{
    const std::array<std::uint8_t, 1> request{{0x01}};
    transport::Metadata encoded;
    ASSERT_TRUE(PackKvRequestEnvelope("192.0.2.10:4502", request.data(), request.size(), encoded)
                    .Success());
    encoded.pop_back();

    KvRequestEnvelopeView decoded;
    EXPECT_TRUE(UnpackKvRequestEnvelope(encoded, decoded).Failure());
}

}  // namespace UC::DRAMPOOL
