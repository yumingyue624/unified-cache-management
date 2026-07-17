/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#include "drampool_buffer.h"
#include <limits>
#include <string>
#include <utility>
#include "buffer_manager.h"
#include "core/transport_manager.h"
#include "drampool_config.h"

namespace UC::DramPool {

Status ToUcStatus(const UC::ASU::Status& status, const char* operation)
{
    if (status.ok()) { return Status::OK(); }

    const auto message = std::string{operation} + ": " + status.message;
    switch (status.code) {
        case UC::ASU::StatusCode::INVALID_ARGUMENT: return Status::InvalidParam(message);
        case UC::ASU::StatusCode::NOT_FOUND: return Status::NotFound();
        case UC::ASU::StatusCode::RESOURCE_BUSY: return Status::NoSpace();
        case UC::ASU::StatusCode::TIMEOUT: return Status::Timeout();
        default: return Status::Error(message);
    }
}

UC::Expected<BufferSlot> AllocateBuffer(DramPoolRuntime& runtime, std::uint32_t len)
{
    if (runtime.bufferManagers.size() != g_config.poolBlockSizes.size()) {
        return Status::Error("buffer manager layout does not match g_config");
    }

    bool foundSuitablePool = false;
    for (std::size_t index = 0; index < runtime.bufferManagers.size(); ++index) {
        if (g_config.poolBlockSizes[index] < len) { continue; }
        foundSuitablePool = true;

        UC::ASU::ScatterGatherEntry sge;
        const auto allocateStatus = runtime.bufferManagers[index]->Allocate(len, sge);
        if (!allocateStatus.ok()) {
            if (allocateStatus.code == UC::ASU::StatusCode::RESOURCE_BUSY) { continue; }
            return ToUcStatus(allocateStatus, "BufferManager::Allocate");
        }

        BufferSlot slot;
        slot.handle =
            BufferHandle{static_cast<std::uint64_t>(sge.slot_index) + 1,
                         static_cast<std::uint32_t>(index), transport::kInvalidMemoryHandle};
        slot.addr = sge.local_addr;
        slot.len = sge.length;
        slot.class_id = static_cast<std::uint32_t>(index);

        // Whole-pool registration will replace this temporary per-slot registration.
        transport::MemoryRegion memory;
        memory.addr = reinterpret_cast<void*>(slot.addr);
        memory.length = slot.len;
        memory.type = transport::MemoryType::Host;
        const auto registerStatus =
            runtime.transport.RegisterMemory(memory, slot.handle.memory_handle);
        if (registerStatus == transport::Status::Ok) { return std::move(slot); }

        (void)runtime.bufferManagers[index]->Free(sge.slot_index);
        return ToUcStatus(registerStatus, "TransportManager::RegisterMemory");
    }

    return foundSuitablePool ? Status::NoSpace()
                             : Status::Error("no buffer pool fits len=" + std::to_string(len));
}

Status FreeBuffer(DramPoolRuntime& runtime, const BufferHandle& handle)
{
    if (!handle.Valid() || handle.class_id >= runtime.bufferManagers.size() ||
        handle.value > std::numeric_limits<std::uint32_t>::max()) {
        return Status::NotFound();
    }
    if (handle.memory_handle != transport::kInvalidMemoryHandle) {
        // The host slot remains owned by BufferManager even if transport is already down.
        (void)runtime.transport.UnregisterMemory(handle.memory_handle);
    }

    const auto slotIndex = static_cast<std::uint32_t>(handle.value - 1);
    return ToUcStatus(runtime.bufferManagers[handle.class_id]->Free(slotIndex),
                      "BufferManager::Free");
}

}  // namespace UC::DramPool
