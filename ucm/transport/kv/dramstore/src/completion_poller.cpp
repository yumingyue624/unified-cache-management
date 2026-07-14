/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#include "completion_poller.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include "drampool_config.h"
#include "logger.h"

namespace UC::DRAMPOOL {
namespace {

std::uint64_t SteadyNowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool IsSuccessful(transport::TransferStatus status)
{ return status == transport::TransferStatus::Completed; }

}  // namespace

void CompletionPoller::Run(const std::atomic_bool& stop)
{
    while (true) {
        if (stop.load(std::memory_order_acquire)) {
            // The formal transport has no per-transfer Cancel; drain each handle safely.
            failAllRequested_.store(true, std::memory_order_release);
        }

        const auto drained = DrainNewHandles();
        const bool stateChanged = PollFirstBatch();

        if (stop.load(std::memory_order_acquire) && drained == 0 && pending_.empty()) { break; }

        if (drained == 0 && !stateChanged) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(g_config.pollerIdleWaitUs));
        }
    }
    pendingCount_.store(pending_.size(), std::memory_order_release);
}

void CompletionPoller::RequestDrainAllAsFailed() noexcept
{ failAllRequested_.store(true, std::memory_order_release); }

std::size_t CompletionPoller::DrainNewHandles()
{
    std::size_t drained = 0;
    while (drained < g_config.pollerDrainBudget && pending_.size() < g_config.pollerMaxPending) {
        InflightRecord record;
        if (!runtime_.transHandleQueue.TryPop(record)) { break; }
        pending_.emplace_back(std::move(record));
        ++drained;
    }
    pendingCount_.store(pending_.size(), std::memory_order_release);
    return drained;
}

bool CompletionPoller::PollFirstBatch()
{
    const std::size_t scanCount =
        std::min(static_cast<std::size_t>(g_config.pollerScanBudget), pending_.size());
    auto iter = pending_.begin();
    bool stateChanged = false;

    // Scan only this head snapshot. Erase never pulls extra work into this round.
    for (std::size_t scanned = 0; scanned < scanCount; ++scanned) {
        transport::TransferStatus transportStatus = transport::TransferStatus::Failed;
        const auto queryStatus = runtime_.transport.GetStatus(iter->handle, transportStatus);
        if (queryStatus != transport::Status::Ok) {
            // GetStatus removes failed handles, so an API failure is also terminal.
            UC_ERROR("CompletionPoller GetStatus failed, handle={}", iter->handle);
            transportStatus = transport::TransferStatus::Failed;
        } else if (transportStatus == transport::TransferStatus::Waiting) {
            const auto nowMs = SteadyNowMs();
            if ((failAllRequested_.load(std::memory_order_acquire) ||
                 OperationTimedOut(*iter, nowMs)) &&
                iter->phase == InflightPhase::Polling) {
                iter->phase = InflightPhase::TimedOut;
                stateChanged = true;
            }
            ++iter;  // WAITING stays at its current position.
            continue;
        }

        if (iter->phase == InflightPhase::TimedOut) {
            transportStatus = transport::TransferStatus::Failed;
        }
        // Formal GetStatus releases terminal handles; settle business state exactly once.
        ApplyTerminal(*iter, transportStatus);
        iter = pending_.erase(iter);
        stateChanged = true;
    }

    pendingCount_.store(pending_.size(), std::memory_order_release);
    return stateChanged;
}

void CompletionPoller::ApplyTerminal(InflightRecord& record,
                                     transport::TransferStatus terminalStatus)
{
    for (const auto& item : record.transfer_items) {
        ResultCode result = ResultCode::Failed;

        // Settle metadata and buffer ownership before completing the request item.
        if (record.opcode == KvOpcode::Dump) {
            if (IsSuccessful(terminalStatus)) {
                const auto status = runtime_.metadata.PublishDump(item.key);
                if (status.Success()) {
                    result = ResultCode::Ok;
                } else {
                    UC_ERROR("CompletionPoller PublishDump failed, handle={}, error={}",
                             record.handle, status);
                }
            } else {
                const auto abortStatus = runtime_.metadata.AbortDump(item.key);
                if (abortStatus.Success()) {
                    const auto freeStatus = FreeBuffer(runtime_, item.buffer_handle);
                    if (freeStatus.Failure()) {
                        UC_ERROR(
                            "CompletionPoller Free failed after DUMP abort, handle={}, error={}",
                            record.handle, freeStatus);
                    }
                } else {
                    UC_ERROR("CompletionPoller AbortDump failed, handle={}, error={}",
                             record.handle, abortStatus);
                }
            }
        } else if (record.opcode == KvOpcode::Load) {
            const auto releaseStatus = runtime_.metadata.ReleaseLoadIo(item.key);
            if (releaseStatus.Failure()) {
                UC_ERROR("CompletionPoller ReleaseLoadIo failed, handle={}, error={}",
                         record.handle, releaseStatus);
            } else if (IsSuccessful(terminalStatus)) {
                result = ResultCode::Ok;
            }
        }

        if (item.index_in_request >= record.results.size()) {
            UC_ERROR("CompletionPoller result index out of range, handle={}, index={}",
                     record.handle, item.index_in_request);
            continue;
        }
        record.results[item.index_in_request] = static_cast<std::uint32_t>(result);
    }

    const auto responseStatus = WriteResponse(runtime_, record.opcode, record.response_addr,
                                              record.peer_manager_id, record.results);
    if (responseStatus.Failure()) {
        UC_ERROR("CompletionPoller WriteResponse failed, handle={}, error={}", record.handle,
                 responseStatus);
    }
}

bool CompletionPoller::OperationTimedOut(const InflightRecord& record, std::uint64_t nowMs) const
{
    if (nowMs < record.submit_ms) { return false; }
    return nowMs - record.submit_ms >= g_config.opTimeoutMs;
}

}  // namespace UC::DRAMPOOL
