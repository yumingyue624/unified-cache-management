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
    explicit CompletionPoller(DramPoolRuntime& runtime) : runtime_(runtime) {}

    void Run(const std::atomic_bool& stop);
    void RequestDrainAllAsFailed() noexcept;

private:
    std::size_t DrainNewHandles();
    bool PollFirstBatch();
    void ApplyTerminal(InflightRecord& record, transport::TransferStatus terminalStatus);
    bool OperationTimedOut(const InflightRecord& record, std::uint64_t nowMs) const;

    DramPoolRuntime& runtime_;
    std::deque<InflightRecord> pending_;
    std::atomic_bool failAllRequested_{false};
};

}  // namespace UC::DRAMPOOL
