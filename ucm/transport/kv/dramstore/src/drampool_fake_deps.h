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

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include "drampool_types.h"
#include "kv_protocol.h"
#include "status/status.h"

namespace UC::DRAMPOOL {

// Temporary contracts for modules not landed yet. Real implementations can
// replace the fake classes without changing TaskWorker or CompletionPoller.

enum class TransportDirection {
    ReadRemoteToLocal = 0,
    WriteLocalToRemote = 1,
};

struct TransportSegment {
    std::uint64_t local_addr{0};
    std::uint64_t remote_addr{0};
    std::uint32_t len{0};
};

struct TransportOp {
    KvOpcode opcode{KvOpcode::None};
    TransportDirection direction{TransportDirection::ReadRemoteToLocal};
    std::vector<TransportSegment> segments;
};

struct BufferSlot {
    BufferHandle handle;
    std::uint64_t addr{0};
    std::uint32_t len{0};
    std::uint32_t class_id{0};
};

struct EntryCreateOptions {
    BlockId key{};
    BufferHandle buffer_handle;
    std::uint64_t local_addr{0};
    std::uint32_t len{0};
    std::uint64_t abs_pos{0};
    std::uint64_t expire_at_ms{0};
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

enum class TransportStatus {
    Waiting = 0,
    Success = 1,
    Failed = 2,
    Canceled = 3,
};

class MetadataIndex {
public:
    virtual ~MetadataIndex() = default;

    // Reserve creates an invisible DUMP entry and holds its I/O ownership.
    virtual ReserveDumpResult ReserveDumpEntry(const EntryCreateOptions& options) = 0;
    // A successful pin must remain valid until ReleaseLoadIo.
    virtual LoadPinResult LookupAndPinLoad(const BlockId& key, std::uint64_t now_ms) = 0;
    virtual LookupCode LookupReady(const BlockId& key, std::uint64_t now_ms) = 0;

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
    virtual void Free(BufferHandle handle) = 0;
};

class TransportManager {
public:
    virtual ~TransportManager() = default;

    virtual UC::Expected<TransportHandle> SubmitAsync(const TransportOp& op) = 0;
    virtual UC::Expected<TransportStatus> QueryStatus(TransportHandle handle) = 0;
    // Cancel only requests termination; QueryStatus still owns terminal detection.
    virtual UC::Status Cancel(TransportHandle handle) = 0;
    virtual UC::Status ReleaseHandle(TransportHandle handle) = 0;
};

class FakeMetadataIndex final : public MetadataIndex {
public:
    ReserveDumpResult ReserveDumpEntry(const EntryCreateOptions& options) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (entries_.find(options.key) != entries_.end()) {
            return {ReserveDumpCode::Exists, UC::Status::DuplicateKey()};
        }

        Entry entry;
        entry.options = options;
        entry.io_refcount = 1;
        entries_.emplace(options.key, std::move(entry));
        return {ReserveDumpCode::Reserved, UC::Status::OK()};
    }

    LoadPinResult LookupAndPinLoad(const BlockId& key, std::uint64_t nowMs) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = entries_.find(key);
        if (iter == entries_.end()) { return {LoadPinCode::NotFound, UC::Status::NotFound()}; }
        auto& entry = iter->second;
        if (!entry.ready) { return {LoadPinCode::NotReady, UC::Status::Retry()}; }
        if (Expired(entry, nowMs)) { return {LoadPinCode::Expired, UC::Status::NotFound()}; }

        ++entry.refcount;
        ++entry.io_refcount;
        LoadPinResult result;
        result.code = LoadPinCode::Pinned;
        result.buffer_handle = entry.options.buffer_handle;
        result.local_addr = entry.options.local_addr;
        result.len = entry.options.len;
        return result;
    }

    LookupCode LookupReady(const BlockId& key, std::uint64_t nowMs) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = entries_.find(key);
        if (iter == entries_.end()) { return LookupCode::NotFound; }
        if (!iter->second.ready) { return LookupCode::NotReady; }
        if (Expired(iter->second, nowMs)) { return LookupCode::Expired; }
        return LookupCode::Ready;
    }

    UC::Status PublishDump(const BlockId& key) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = entries_.find(key);
        if (iter == entries_.end()) { return UC::Status::NotFound(); }
        auto& entry = iter->second;
        if (entry.ready || entry.io_refcount != 1) {
            return UC::Status::Error("invalid RESERVED entry while publishing DUMP");
        }
        entry.ready = true;
        --entry.io_refcount;
        return UC::Status::OK();
    }

    UC::Status AbortDump(const BlockId& key) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = entries_.find(key);
        if (iter == entries_.end()) { return UC::Status::NotFound(); }
        if (iter->second.ready) { return UC::Status::Error("cannot abort a READY entry"); }
        entries_.erase(iter);
        return UC::Status::OK();
    }

    UC::Status ReleaseLoadIo(const BlockId& key) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = entries_.find(key);
        if (iter == entries_.end()) { return UC::Status::NotFound(); }
        auto& entry = iter->second;
        if (!entry.ready || entry.refcount == 0 || entry.io_refcount == 0) {
            return UC::Status::Error("invalid LOAD reference counters");
        }
        --entry.refcount;
        --entry.io_refcount;
        return UC::Status::OK();
    }

private:
    struct Entry {
        EntryCreateOptions options;
        std::uint32_t refcount{0};
        std::uint32_t io_refcount{0};
        bool ready{false};
    };

    static bool Expired(const Entry& entry, std::uint64_t nowMs)
    { return entry.options.expire_at_ms != 0 && nowMs >= entry.options.expire_at_ms; }

    std::mutex mutex_;
    std::unordered_map<BlockId, Entry, UC::Detail::BlockIdHasher> entries_;
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

class FakeTransportManager final : public TransportManager {
public:
    UC::Expected<TransportHandle> SubmitAsync(const TransportOp& op) override
    {
        if (op.segments.empty()) {
            return UC::Status::InvalidParam("transport operation has no segments");
        }
        std::lock_guard<std::mutex> guard(mutex_);
        TransportHandle handle{nextHandle_++};
        handles_.emplace(handle.value, TransportStatus::Success);
        return std::move(handle);
    }

    UC::Expected<TransportStatus> QueryStatus(TransportHandle handle) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = handles_.find(handle.value);
        if (iter == handles_.end()) { return UC::Status::NotFound(); }
        ++queryCounts_[handle.value];
        auto status = iter->second;
        return std::move(status);
    }

    UC::Status Cancel(TransportHandle handle) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = handles_.find(handle.value);
        if (iter == handles_.end()) { return UC::Status::NotFound(); }
        if (iter->second == TransportStatus::Waiting) { iter->second = TransportStatus::Canceled; }
        return UC::Status::OK();
    }

    UC::Status ReleaseHandle(TransportHandle handle) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (handles_.erase(handle.value) != 1) { return UC::Status::NotFound(); }
        return UC::Status::OK();
    }

    UC::Status SetStatus(TransportHandle handle, TransportStatus status)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = handles_.find(handle.value);
        if (iter == handles_.end()) { return UC::Status::NotFound(); }
        iter->second = status;
        return UC::Status::OK();
    }

    std::size_t QueryCount(TransportHandle handle) const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto iter = queryCounts_.find(handle.value);
        return iter == queryCounts_.end() ? 0 : iter->second;
    }

    std::size_t ActiveHandleCount() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return handles_.size();
    }

private:
    mutable std::mutex mutex_;
    std::uint64_t nextHandle_{1};
    std::unordered_map<std::uint64_t, TransportStatus> handles_;
    std::unordered_map<std::uint64_t, std::size_t> queryCounts_;
};

inline UC::Status WriteResponse(KvOpcode opcode, std::uint64_t resp_addr,
                                const std::vector<std::uint32_t>& results)
{
    auto len = results.size() * sizeof(std::uint32_t);
    std::vector<std::uint8_t> respbuf(len);
    auto status = g_services.protocol_mgr->PackResponse(respbuf.data(), opcode, KvResponse{results});
    if (status.Failure()) return status;

    TransportOp op;
    op.opcode = opcode;
    op.direction = TransportDirection::WriteLocalToRemote;
    op.segments.push_back({reinterpret_cast<std::uint64_t>(respbuf.data()), resp_addr, len});
    auto submitted = g_services.transport->SubmitAsync(op);
    return submitted.HasValue() ? UC::Status::OK() : submitted.Error();
}

inline UC::Expected<BufferSlot> AllocateBuffer(std::uint32_t len)
{
    for (std::size_t i = 0; i < g_services.buffer_managers.size(); ++i) {
        if (g_services.buffer_managers[i]->SlotSize() >= len) {
            auto result = g_services.buffer_managers[i]->Allocate(len);
            if (result.HasValue()) {
                result.Value().class_id = static_cast<std::uint32_t>(i);
                result.Value().handle.class_id = static_cast<std::uint32_t>(i);
            }
            return result;
        }
    }
    return UC::Status::Error("no buffer pool fits len=" + std::to_string(len));
}

inline UC::Status FreeBuffer(const BufferHandle& handle)
{
    if (handle.class_id < g_services.buffer_managers.size()) {
        return g_services.buffer_managers[handle.class_id]->Free(handle);
    }
    return UC::Status::NotFound();
}

}  // namespace UC::DRAMPOOL
