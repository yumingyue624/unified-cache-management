/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#include "drampool_buffer.h"
#include <limits>
#include <string>
#include <utility>
#include "drampool_config.h"
#include "pool/buffer_pool.h"

namespace UC::DramPool {

UC::Expected<BufferSlot> AllocateBuffer(DramPoolRuntime& runtime, std::uint32_t len)
{
    if (runtime.bufferPools.size() != g_config.poolBlockSizes.size()) {
        return Status::Error("buffer pool layout does not match g_config");
    }

    bool foundSuitablePool = false;
    for (std::size_t index = 0; index < runtime.bufferPools.size(); ++index) {
        if (g_config.poolBlockSizes[index] < len) { continue; }
        foundSuitablePool = true;

        UC::BufferPool::Slot poolSlot;
        const auto allocateStatus = runtime.bufferPools[index]->Allocate(poolSlot);
        if (allocateStatus.Failure()) {
            if (allocateStatus == Status::Retry()) { continue; }
            return allocateStatus;
        }

        BufferSlot slot;
        slot.handle = BufferHandle{static_cast<std::uint64_t>(poolSlot.slot_index) + 1,
                                   static_cast<std::uint32_t>(index)};
        slot.addr = reinterpret_cast<std::uint64_t>(poolSlot.local_addr);
        slot.len = len;
        slot.class_id = static_cast<std::uint32_t>(index);
        return std::move(slot);
    }

    return foundSuitablePool ? Status::NoSpace()
                             : Status::Error("no buffer pool fits len=" + std::to_string(len));
}

Status FreeBuffer(DramPoolRuntime& runtime, const BufferHandle& handle)
{
    if (!handle.Valid() || handle.class_id >= runtime.bufferPools.size() ||
        handle.value > std::numeric_limits<std::uint32_t>::max()) {
        return Status::NotFound();
    }
    const auto slotIndex = static_cast<std::uint32_t>(handle.value - 1);
    return runtime.bufferPools[handle.class_id]->Free(slotIndex);
}

}  // namespace UC::DramPool
