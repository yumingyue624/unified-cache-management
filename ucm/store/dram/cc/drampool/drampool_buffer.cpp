/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#include "drampool_buffer.h"
#include <limits>
#include <string>
#include <utility>
#include "core/transport_manager.h"
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
        slot.handle =
            BufferHandle{static_cast<std::uint64_t>(poolSlot.slot_index) + 1,
                         static_cast<std::uint32_t>(index), transport::kInvalidMemoryHandle};
        slot.addr = reinterpret_cast<std::uint64_t>(poolSlot.local_addr);
        slot.len = len;
        slot.class_id = static_cast<std::uint32_t>(index);

        // Whole-pool registration will replace this temporary per-slot registration.
        transport::MemoryRegion memory;
        memory.addr = reinterpret_cast<void*>(slot.addr);
        memory.length = slot.len;
        memory.type = transport::MemoryType::Host;
        const auto registerStatus =
            runtime.transport.RegisterMemory(memory, slot.handle.memory_handle);
        if (registerStatus == transport::Status::Ok) { return std::move(slot); }

        (void)runtime.bufferPools[index]->Free(poolSlot.slot_index);
        return ToUcStatus(registerStatus, "TransportManager::RegisterMemory");
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
    if (handle.memory_handle != transport::kInvalidMemoryHandle) {
        // The host slot remains owned by BufferPool even if transport is already down.
        (void)runtime.transport.UnregisterMemory(handle.memory_handle);
    }

    const auto slotIndex = static_cast<std::uint32_t>(handle.value - 1);
    return runtime.bufferPools[handle.class_id]->Free(slotIndex);
}

}  // namespace UC::DramPool
