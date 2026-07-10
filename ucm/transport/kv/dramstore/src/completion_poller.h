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

struct CompletionPollerOptions {
    std::size_t drain_budget{0};
    std::size_t scan_budget{0};
    std::size_t max_pending{0};
    std::uint64_t operation_timeout_ms{0};
    std::uint32_t idle_wait_us{0};
};

class CompletionPoller final {
public:
    CompletionPoller(TransHandleQueue& ingress, MetadataIndex& metadata,
                     BufferManager& bufferManager, TransportManager& transport,
                     ResponseWriter& responseWriter, CompletionPollerOptions options);

    void Run(const std::atomic_bool& stop) noexcept;
    void RequestCancelAll() noexcept;

    bool Healthy() const noexcept { return healthy_.load(std::memory_order_acquire); }
    std::size_t PendingCount() const noexcept
    { return pendingCount_.load(std::memory_order_acquire); }

private:
    std::size_t DrainNewHandles();
    bool PollFirstBatch();
    bool RequestCancel(InflightRecord& record);
    void ApplyTerminal(InflightRecord& record, TransportStatus terminalStatus);
    bool TryReleaseHandle(InflightRecord& record);
    bool OperationTimedOut(const InflightRecord& record, std::uint64_t nowMs) const;

    TransHandleQueue& ingress_;
    MetadataIndex& metadata_;
    BufferManager& bufferManager_;
    TransportManager& transport_;
    ResponseWriter& responseWriter_;
    CompletionPollerOptions options_;

    std::deque<InflightRecord> pending_;
    std::atomic_bool cancelAllRequested_{false};
    std::atomic_bool healthy_{true};
    std::atomic_size_t pendingCount_{0};
};

}  // namespace UC::DRAMPOOL
