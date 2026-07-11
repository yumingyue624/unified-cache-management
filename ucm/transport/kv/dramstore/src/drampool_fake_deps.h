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
 * DEALINGS IN THE SOFTWARE.
 * */
#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include "drampool_transport.h"
#include "drampool_types.h"
#include "entry.h"
#include "kv_protocol.h"
#include "status/status.h"

namespace UC::DRAMPOOL {

// Temporary contracts for modules not landed yet. Real implementations can
// replace the fake classes without changing TaskWorker or CompletionPoller.

struct BufferSlot {
    BufferHandle handle;
    std::uint64_t addr{0};
    std::uint32_t len{0};
    std::uint32_t class_id{0};
};

enum class ReserveDumpCode {
    Reserved = 0,
    Exists = 1,
    Failed = 2,
};

struct ReserveDumpResult {
    ReserveDumpCode code{ReserveDumpCode::Failed};
    UC::Status status{UC::Status::OK()};
};

enum class LoadPinCode {
    Pinned = 0,
    NotFound = 1,
    NotReady = 2,
    Expired = 3,
    Failed = 4,
};

struct LoadPinResult {
    LoadPinCode code{LoadPinCode::Failed};
    UC::Status status{UC::Status::OK()};
    BufferHandle buffer_handle;
    std::uint64_t local_addr{0};
    std::uint32_t len{0};
};

enum class LookupCode {
    Ready = 0,
    NotFound = 1,
    NotReady = 2,
    Expired = 3,
    Failed = 4,
};

class MetadataIndex {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    virtual ~MetadataIndex() = default;

    // Reserve creates an invisible DUMP entry and holds its I/O ownership.
    virtual ReserveDumpResult ReserveDumpEntry(const UC::DramStore::EntryPtr& entry,
                                               const BufferHandle& bufferHandle) = 0;
    // A successful pin must remain valid until ReleaseLoadIo.
    virtual LoadPinResult LookupAndPinLoad(const BlockId& key, TimePoint now) = 0;
    virtual LookupCode LookupReady(const BlockId& key, TimePoint now) = 0;

    // These calls atomically update all metadata indexes and entry state.
    virtual UC::Status PublishDump(const BlockId& key) = 0;
    virtual UC::Status AbortDump(const BlockId& key) = 0;
    virtual UC::Status ReleaseLoadIo(const BlockId& key) = 0;
};

class BufferManager {
public:
    virtual ~BufferManager() = default;

    virtual std::uint32_t SlotSize() const = 0;
    virtual UC::Expected<BufferSlot> Allocate(std::uint32_t len) = 0;
    virtual UC::Status Free(BufferHandle handle) = 0;
};

class FakeMetadataIndex final : public MetadataIndex {
public:
    ReserveDumpResult ReserveDumpEntry(const UC::DramStore::EntryPtr& entry,
                                       const BufferHandle& bufferHandle) override
    {
        if (!entry || !bufferHandle.Valid()) {
            return {ReserveDumpCode::Failed, UC::Status::InvalidParam("invalid DUMP entry")};
        }

        std::lock_guard<std::mutex> guard(mutex_);
        if (entries_.find(entry->key) != entries_.end()) {
            return {ReserveDumpCode::Exists, UC::Status::DuplicateKey()};
        }

        UC::SpinLockGuard entryGuard(entry->lock);
        if (entry->status != UC::DramStore::EntryStatus::INITIALIZED || entry->refCnt != 0) {
            return {ReserveDumpCode::Failed,
                    UC::Status::InvalidParam("DUMP entry is not initialized")};
        }

        // The initial reference protects the buffer until DUMP reaches a terminal state.
        entry->refCnt = 1;
        entries_.emplace(entry->key, entry);
        bufferHandles_.emplace(entry->key, bufferHandle);
        return {ReserveDumpCode::Reserved, UC::Status::OK()};
    }

    LoadPinResult LookupAndPinLoad(const BlockId& key, TimePoint now) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = entries_.find(key);
        if (iter == entries_.end()) { return {LoadPinCode::NotFound, UC::Status::NotFound()}; }
        auto& entry = *iter->second;
        UC::SpinLockGuard entryGuard(entry.lock);
        if (entry.status != UC::DramStore::EntryStatus::READY) {
            return {LoadPinCode::NotReady, UC::Status::Retry()};
        }
        if (Expired(entry, now)) { return {LoadPinCode::Expired, UC::Status::NotFound()}; }
        if (entry.size > std::numeric_limits<std::uint32_t>::max()) {
            return {LoadPinCode::Failed, UC::Status::InvalidParam("entry size exceeds protocol")};
        }
        auto handleIter = bufferHandles_.find(key);
        if (handleIter == bufferHandles_.end()) {
            return {LoadPinCode::Failed, UC::Status::Error("entry buffer handle is missing")};
        }

        ++entry.refCnt;
        LoadPinResult result;
        result.code = LoadPinCode::Pinned;
        result.buffer_handle = handleIter->second;
        result.local_addr = reinterpret_cast<std::uintptr_t>(entry.addr);
        result.len = static_cast<std::uint32_t>(entry.size);
        return result;
    }

    LookupCode LookupReady(const BlockId& key, TimePoint now) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = entries_.find(key);
        if (iter == entries_.end()) { return LookupCode::NotFound; }
        auto& entry = *iter->second;
        UC::SpinLockGuard entryGuard(entry.lock);
        if (entry.status != UC::DramStore::EntryStatus::READY) { return LookupCode::NotReady; }
        if (Expired(entry, now)) { return LookupCode::Expired; }
        return LookupCode::Ready;
    }

    UC::Status PublishDump(const BlockId& key) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = entries_.find(key);
        if (iter == entries_.end()) { return UC::Status::NotFound(); }
        auto& entry = *iter->second;
        UC::SpinLockGuard entryGuard(entry.lock);
        if (entry.status != UC::DramStore::EntryStatus::INITIALIZED || entry.refCnt != 1) {
            return UC::Status::Error("invalid RESERVED entry while publishing DUMP");
        }
        entry.status = UC::DramStore::EntryStatus::READY;
        --entry.refCnt;
        return UC::Status::OK();
    }

    UC::Status AbortDump(const BlockId& key) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = entries_.find(key);
        if (iter == entries_.end()) { return UC::Status::NotFound(); }
        auto entry = iter->second;
        UC::SpinLockGuard entryGuard(entry->lock);
        if (entry->status == UC::DramStore::EntryStatus::READY) {
            return UC::Status::Error("cannot abort a READY entry");
        }
        entry->status = UC::DramStore::EntryStatus::DELETING;
        entry->refCnt = 0;
        bufferHandles_.erase(key);
        entries_.erase(iter);
        return UC::Status::OK();
    }

    UC::Status ReleaseLoadIo(const BlockId& key) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = entries_.find(key);
        if (iter == entries_.end()) { return UC::Status::NotFound(); }
        auto& entry = *iter->second;
        UC::SpinLockGuard entryGuard(entry.lock);
        if (entry.status != UC::DramStore::EntryStatus::READY || entry.refCnt == 0) {
            return UC::Status::Error("invalid LOAD reference count");
        }
        --entry.refCnt;
        return UC::Status::OK();
    }

private:
    static bool Expired(const UC::DramStore::Entry& entry, TimePoint now)
    { return entry.lifeTimeout != TimePoint{} && now >= entry.lifeTimeout; }

    std::mutex mutex_;
    std::unordered_map<BlockId, UC::DramStore::EntryPtr, UC::Detail::BlockIdHasher> entries_;
    std::unordered_map<BlockId, BufferHandle, UC::Detail::BlockIdHasher> bufferHandles_;
};

class FakeBufferManager final : public BufferManager {
public:
    explicit FakeBufferManager(std::uint32_t slotSize) : slotSize_(slotSize) {}

    std::uint32_t SlotSize() const override { return slotSize_; }

    UC::Expected<BufferSlot> Allocate(std::uint32_t len) override
    {
        if (len == 0) { return UC::Status::InvalidParam("buffer length must be positive"); }
        auto storage = std::make_unique<std::uint8_t[]>(len);

        std::lock_guard<std::mutex> guard(mutex_);
        const std::uint64_t id = nextId_++;
        BufferSlot slot;
        slot.handle = BufferHandle{id, 0};
        slot.addr = reinterpret_cast<std::uintptr_t>(storage.get());
        slot.len = len;
        allocations_.emplace(id, std::move(storage));
        return std::move(slot);
    }

    UC::Status Free(BufferHandle handle) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!handle.Valid() || allocations_.erase(handle.value) != 1) {
            return UC::Status::NotFound();
        }
        return UC::Status::OK();
    }

    std::size_t ActiveAllocationCount() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return allocations_.size();
    }

private:
    std::uint32_t slotSize_{0};
    mutable std::mutex mutex_;
    std::uint64_t nextId_{1};
    std::unordered_map<std::uint64_t, std::unique_ptr<std::uint8_t[]>> allocations_;
};

inline UC::Expected<BufferSlot> AllocateBuffer(std::uint32_t len);
inline UC::Status FreeBuffer(const BufferHandle& handle);

inline UC::Status WriteResponse(KvOpcode opcode, std::uint64_t resp_addr,
                                const transport::ManagerID& peer_manager_id,
                                const std::vector<std::uint32_t>& results)
{
    if (peer_manager_id.empty()) {
        return UC::Status::InvalidParam("response peer_manager_id is empty");
    }
    const auto len = static_cast<std::uint32_t>(results.size() * sizeof(std::uint32_t));
    auto allocated = AllocateBuffer(len);
    if (!allocated.HasValue()) { return allocated.Error(); }
    auto slot = std::move(allocated).Value();

    const auto protocol_status = g_services.protocol_mgr->PackResponse(
        reinterpret_cast<void*>(slot.addr), opcode, KvResponse{results});
    UC::Status status =
        protocol_status.ok() ? UC::Status::OK() : UC::Status::Error(protocol_status.message);
    if (status.Success()) {
        transport::Operation operation;
        operation.opcode = transport::Opcode::Write;
        operation.direct = transport::OperationDirect::RemoteDeviceHost;
        operation.target_manager = peer_manager_id;
        operation.ops.push_back(
            transport::Segment{reinterpret_cast<void*>(slot.addr), resp_addr, len});
        status = ToUcStatus(g_services.transport->ExecuteSync(operation), "ExecuteSync response");
    }

    const auto free_status = FreeBuffer(slot.handle);
    return status.Failure() ? status : free_status;
}

inline UC::Expected<BufferSlot> AllocateBuffer(std::uint32_t len)
{
    for (std::size_t i = 0; i < g_services.buffer_managers.size(); ++i) {
        if (g_services.buffer_managers[i]->SlotSize() >= len) {
            auto result = g_services.buffer_managers[i]->Allocate(len);
            if (result.HasValue()) {
                result.Value().class_id = static_cast<std::uint32_t>(i);
                result.Value().handle.class_id = static_cast<std::uint32_t>(i);
                transport::MemoryRegion memory;
                memory.addr = reinterpret_cast<void*>(result.Value().addr);
                memory.length = result.Value().len;
                memory.type = transport::MemoryType::Host;
                auto register_status = g_services.transport->RegisterMemory(
                    memory, result.Value().handle.memory_handle);
                if (register_status != transport::Status::Ok) {
                    const auto handle = result.Value().handle;
                    (void)g_services.buffer_managers[i]->Free(handle);
                    return ToUcStatus(register_status, "RegisterMemory");
                }
            }
            return result;
        }
    }
    return UC::Status::Error("no buffer pool fits len=" + std::to_string(len));
}

inline UC::Status FreeBuffer(const BufferHandle& handle)
{
    if (handle.class_id < g_services.buffer_managers.size()) {
        if (handle.memory_handle != transport::kInvalidMemoryHandle) {
            // Unregister may fail after TransportManager::Shutdown; the host buffer is still freed.
            (void)g_services.transport->UnregisterMemory(handle.memory_handle);
        }
        return g_services.buffer_managers[handle.class_id]->Free(handle);
    }
    return UC::Status::NotFound();
}

}  // namespace UC::DRAMPOOL
