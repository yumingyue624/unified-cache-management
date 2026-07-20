/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#include "drampool_buffer.h"
#include <algorithm>
#include "drampool_config.h"

namespace UC::DramPool {

Status AcquireBufferAtLeast(BufferManager& manager, std::size_t size, BufferLease& lease)
{
    if (lease) { return Status::InvalidParam("buffer lease is already active"); }
    auto poolSizes = g_config.poolBlockSizes;
    std::sort(poolSizes.begin(), poolSizes.end());

    bool foundSuitablePool = false;
    for (const auto poolSize : poolSizes) {
        if (poolSize < size) { continue; }
        foundSuitablePool = true;
        auto status = AcquireBuffer(manager, static_cast<std::size_t>(poolSize), lease);
        if (status.Success()) { return status; }
        if (status != Status::NoSpace()) { return status; }
    }
    return foundSuitablePool ? Status::NoSpace() : Status::NotFound();
}

}  // namespace UC::DramPool
