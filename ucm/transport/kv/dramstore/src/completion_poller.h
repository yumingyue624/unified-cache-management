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
    std::size_t DrainNewCompletions();
    void PollPendingCompletions();
    // Returns true after the data transfer has been settled and the record is ready
    // to advance to response submission. Waiting transfers remain pending.
    bool ProcessDataTransfer(CompletionRecord& record);
    UC::Status SubmitResponse(CompletionRecord& record);
    bool ProcessResponseTransfer(CompletionRecord& record);
    void SettleDataTransfer(CompletionRecord& record,
                            transport::TransferStatus terminalStatus);
    bool OperationTimedOut(const CompletionRecord& record, std::uint64_t nowMs) const;

    DramPoolRuntime& runtime_;
    std::deque<CompletionRecord> pending_;
    std::atomic_bool failAllRequested_{false};
};

}  // namespace UC::DRAMPOOL
