/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#pragma once

#include <string>
#include "core/transport_manager.h"
#include "status/status.h"

namespace UC::DRAMPOOL {

inline UC::Status ToUcStatus(transport::Status status, const char* operation)
{
    if (status == transport::Status::Ok) { return UC::Status::OK(); }
    if (status == transport::Status::InvalidArgument) {
        return UC::Status::InvalidParam("{}: invalid transport argument", operation);
    }
    return UC::Status::Error(std::string{operation} + ": transport operation failed");
}

}  // namespace UC::DRAMPOOL
