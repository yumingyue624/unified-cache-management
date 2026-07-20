/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#pragma once

#include <cstddef>
#include <utility>
#include "buffer.h"
#include "buffer_manager.h"
#include "status/status.h"

namespace UC::DramPool {

class BufferLease {
public:
    BufferLease() = default;
    ~BufferLease() { (void)Reset(); }

    BufferLease(const BufferLease&) = delete;
    BufferLease& operator=(const BufferLease&) = delete;

    BufferLease(BufferLease&& other) noexcept { MoveFrom(std::move(other)); }
    BufferLease& operator=(BufferLease&& other) noexcept
    {
        if (this != &other) {
            (void)Reset();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    explicit operator bool() const noexcept { return manager_ != nullptr; }
    const Buffer& Get() const noexcept { return buffer_; }
    Buffer& Get() noexcept { return buffer_; }
    Status Reset() noexcept;

private:
    friend Status AcquireBuffer(BufferManager&, std::size_t, BufferLease&);

    void MoveFrom(BufferLease&& other) noexcept
    {
        manager_ = std::exchange(other.manager_, nullptr);
        buffer_ = std::exchange(other.buffer_, {});
    }

    BufferManager* manager_{nullptr};
    Buffer buffer_{};
};

Status AcquireBuffer(BufferManager& manager, std::size_t size, BufferLease& lease);
Status AcquireBufferAtLeast(BufferManager& manager, std::size_t size, BufferLease& lease);

inline Status BufferLease::Reset() noexcept
{
    auto* manager = std::exchange(manager_, nullptr);
    const auto buffer = std::exchange(buffer_, {});
    if (manager == nullptr) { return Status::OK(); }
    return manager->Free(buffer.length, buffer.slot);
}

inline Status AcquireBuffer(BufferManager& manager, std::size_t size, BufferLease& lease)
{
    if (lease) { return Status::InvalidParam("buffer lease is already active"); }
    Buffer buffer;
    auto status = manager.Allocate(size, buffer);
    if (status.Success()) {
        lease.manager_ = &manager;
        lease.buffer_ = std::move(buffer);
    }
    return status;
}

}  // namespace UC::DramPool
