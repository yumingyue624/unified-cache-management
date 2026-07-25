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
#include "drampool_types.h"

namespace UC::DramPool {

class CompletionPoller final {
public:
    explicit CompletionPoller(DramPoolRuntime& runtime) : runtime_(runtime) {}

    void Run(const std::atomic_bool& stop);

private:
    std::size_t FillPendingWindow();
    void PollPendingCompletions();
    // Returns true after the data transfer has been settled and the record is ready
    // to advance to response submission. Waiting transfers remain pending.
    bool PollDataTransfer(CompletionRecord& record);
    bool SubmitResponse(CompletionRecord& record);
    bool PollResponseTransfer(CompletionRecord& record);
    void SettleDataTransfer(CompletionRecord& record, transport::TransferStatus terminalStatus);
    bool OperationTimedOut(const CompletionRecord& record, std::uint64_t nowMs) const;
    void DisconnectPeer(const transport::ManagerID& peer, TransportHandle handle);

    DramPoolRuntime& runtime_;
    std::deque<CompletionRecord> pending_;
    bool disconnectAllTransfers_{false};
};

}  // namespace UC::DramPool
