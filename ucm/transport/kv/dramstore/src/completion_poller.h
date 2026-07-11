/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include "drampool_fake_deps.h"
#include "drampool_types.h"

namespace UC::DRAMPOOL {

class CompletionPoller final {
public:
    CompletionPoller() = default;

    void Run(const std::atomic_bool& stop) noexcept;
    void RequestDrainAllAsFailed() noexcept;

    bool Healthy() const noexcept { return healthy_.load(std::memory_order_acquire); }
    std::size_t PendingCount() const noexcept
    { return pendingCount_.load(std::memory_order_acquire); }

private:
    std::size_t DrainNewHandles();
    bool PollFirstBatch();
    void ApplyTerminal(InflightRecord& record, transport::TransferStatus terminalStatus);
    bool OperationTimedOut(const InflightRecord& record, std::uint64_t nowMs) const;

    std::deque<InflightRecord> pending_;
    std::atomic_bool failAllRequested_{false};
    std::atomic_bool healthy_{true};
    std::atomic_size_t pendingCount_{0};
};

}  // namespace UC::DRAMPOOL
