/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#pragma once

#include <cstdint>
#include "drampool_types.h"
#include "status/status.h"

namespace UC::DramPool {

UC::Expected<BufferSlot> AllocateBuffer(DramPoolRuntime& runtime, std::uint32_t len);
Status FreeBuffer(DramPoolRuntime& runtime, const BufferHandle& handle);

}  // namespace UC::DramPool
