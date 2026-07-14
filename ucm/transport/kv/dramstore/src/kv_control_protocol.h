/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#pragma once

#include <cstddef>
#include "core/transport.h"
#include "status/status.h"

namespace UC::DRAMPOOL {

// TCP control metadata stays outside the KV wire protocol.
struct KvRequestEnvelopeView {
    transport::ManagerID peer_manager_id;
    const void* request_data{nullptr};
    std::size_t request_size{0};
};

UC::Status PackKvRequestEnvelope(const transport::ManagerID& peerManagerId,
                                 const void* requestData, std::size_t requestSize,
                                 transport::Metadata& output);
UC::Status UnpackKvRequestEnvelope(const transport::Metadata& input,
                                   KvRequestEnvelopeView& output);

}  // namespace UC::DRAMPOOL
