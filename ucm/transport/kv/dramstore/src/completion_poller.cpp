/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#include "completion_poller.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include "logger.h"

namespace UC::DRAMPOOL {
namespace {

std::uint64_t SteadyNowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool IsSuccessful(TransportStatus status) { return status == TransportStatus::Success; }

}  // namespace

CompletionPoller::CompletionPoller(TransHandleQueue& ingress, MetadataIndex& metadata,
                                   BufferManager& bufferManager, TransportManager& transport,
                                   ResponseWriter& responseWriter, CompletionPollerOptions options)
    : ingress_(ingress),
      metadata_(metadata),
      bufferManager_(bufferManager),
      transport_(transport),
      responseWriter_(responseWriter),
      options_(options)
{
    if (options_.drain_budget == 0 || options_.scan_budget == 0 || options_.max_pending == 0 ||
        options_.operation_timeout_ms == 0 || options_.idle_wait_us == 0) {
        throw std::invalid_argument("CompletionPoller options must be positive");
    }
    if (options_.max_pending < options_.drain_budget ||
        options_.max_pending < options_.scan_budget) {
        throw std::invalid_argument("CompletionPoller max_pending is smaller than a work budget");
    }
}

void CompletionPoller::Run(const std::atomic_bool& stop) noexcept
{
    try {
        while (true) {
            if (stop.load(std::memory_order_acquire)) {
                // Shutdown requests cancellation, then polls until every handle is terminal.
                cancelAllRequested_.store(true, std::memory_order_release);
            }

            const auto drained = DrainNewHandles();
            const bool stateChanged = PollFirstBatch();

            if (stop.load(std::memory_order_acquire) && drained == 0 && pending_.empty()) { break; }

            if (drained == 0 && !stateChanged) {
                std::this_thread::sleep_for(std::chrono::microseconds(options_.idle_wait_us));
            }
        }
    } catch (const std::exception& error) {
        healthy_.store(false, std::memory_order_release);
        UC_ERROR_UNLIMITED("CompletionPoller stopped by exception: {}", error.what());
    } catch (...) {
        healthy_.store(false, std::memory_order_release);
        UC_ERROR_UNLIMITED("CompletionPoller stopped by unknown exception");
    }
    pendingCount_.store(pending_.size(), std::memory_order_release);
}

void CompletionPoller::RequestCancelAll() noexcept
{ cancelAllRequested_.store(true, std::memory_order_release); }

std::size_t CompletionPoller::DrainNewHandles()
{
    std::size_t drained = 0;
    while (drained < options_.drain_budget && pending_.size() < options_.max_pending) {
        InflightRecord record;
        if (!ingress_.TryPop(record)) { break; }
        pending_.emplace_back(std::move(record));
        ++drained;
    }
    pendingCount_.store(pending_.size(), std::memory_order_release);
    return drained;
}

bool CompletionPoller::PollFirstBatch()
{
    const std::size_t scanCount = std::min(options_.scan_budget, pending_.size());
    auto iter = pending_.begin();
    bool stateChanged = false;

    // Scan only this head snapshot. Erase never pulls extra work into this round.
    for (std::size_t scanned = 0; scanned < scanCount; ++scanned) {
        if (iter->phase == InflightPhase::AppliedAwaitingRelease) {
            if (TryReleaseHandle(*iter)) {
                iter = pending_.erase(iter);
                stateChanged = true;
            } else {
                ++iter;
            }
            continue;
        }

        auto queried = transport_.QueryStatus(iter->handle);
        if (!queried.HasValue()) {
            UC_ERROR("CompletionPoller QueryStatus failed, handle={}, error={}", iter->handle.value,
                     queried.Error());
            const auto nowMs = SteadyNowMs();
            if ((cancelAllRequested_.load(std::memory_order_acquire) ||
                 OperationTimedOut(*iter, nowMs)) &&
                iter->phase == InflightPhase::Polling) {
                stateChanged = RequestCancel(*iter) || stateChanged;
            }
            ++iter;
            continue;
        }

        const auto transportStatus = std::move(queried).Value();
        if (transportStatus == TransportStatus::Waiting) {
            const auto nowMs = SteadyNowMs();
            if ((cancelAllRequested_.load(std::memory_order_acquire) ||
                 OperationTimedOut(*iter, nowMs)) &&
                iter->phase == InflightPhase::Polling) {
                stateChanged = RequestCancel(*iter) || stateChanged;
            }
            ++iter;  // WAITING stays at its current position.
            continue;
        }

        // Apply metadata once; a failed handle release retries without replaying it.
        ApplyTerminal(*iter, transportStatus);
        iter->phase = InflightPhase::AppliedAwaitingRelease;
        stateChanged = true;
        if (TryReleaseHandle(*iter)) {
            iter = pending_.erase(iter);
        } else {
            ++iter;
        }
    }

    pendingCount_.store(pending_.size(), std::memory_order_release);
    return stateChanged;
}

bool CompletionPoller::RequestCancel(InflightRecord& record)
{
    // Cancel is asynchronous; keep the record until QueryStatus returns a terminal state.
    const auto status = transport_.Cancel(record.handle);
    if (status.Failure()) {
        UC_ERROR("CompletionPoller Cancel failed, handle={}, error={}", record.handle.value,
                 status);
        return false;
    }
    record.phase = InflightPhase::CancelRequested;
    return true;
}

void CompletionPoller::ApplyTerminal(InflightRecord& record, TransportStatus terminalStatus)
{
    for (const auto& item : record.transfer_items) {
        ResultCode result = ResultCode::Failed;

        // Settle metadata and buffer ownership before completing the request item.
        if (record.opcode == KvOpcode::Dump) {
            if (IsSuccessful(terminalStatus)) {
                const auto status = metadata_.PublishDump(item.key);
                if (status.Success()) {
                    result = ResultCode::Ok;
                } else {
                    UC_ERROR("CompletionPoller PublishDump failed, handle={}, error={}",
                             record.handle.value, status);
                }
            } else {
                const auto abortStatus = metadata_.AbortDump(item.key);
                if (abortStatus.Success()) {
                    const auto freeStatus = bufferManager_.Free(item.buffer_handle);
                    if (freeStatus.Failure()) {
                        UC_ERROR(
                            "CompletionPoller Free failed after DUMP abort, handle={}, error={}",
                            record.handle.value, freeStatus);
                    }
                } else {
                    UC_ERROR("CompletionPoller AbortDump failed, handle={}, error={}",
                             record.handle.value, abortStatus);
                }
            }
        } else if (record.opcode == KvOpcode::Load) {
            const auto releaseStatus = metadata_.ReleaseLoadIo(item.key);
            if (releaseStatus.Failure()) {
                UC_ERROR("CompletionPoller ReleaseLoadIo failed, handle={}, error={}",
                         record.handle.value, releaseStatus);
            } else if (IsSuccessful(terminalStatus)) {
                result = ResultCode::Ok;
            }
        }

        if (!record.request_ctx) {
            UC_ERROR("CompletionPoller record has no RequestContext, handle={}",
                     record.handle.value);
            continue;
        }

        std::optional<FinalResponse> finalResponse;
        const auto completeStatus =
            record.request_ctx->CompleteItem(item.request_index, result, finalResponse);
        if (completeStatus.Failure()) {
            UC_ERROR("CompletionPoller CompleteItem failed, handle={}, error={}",
                     record.handle.value, completeStatus);
            continue;
        }
        if (!finalResponse.has_value()) { continue; }

        const auto responseStatus = responseWriter_.WriteResponse(
            finalResponse->opcode, finalResponse->response_addr, finalResponse->results);
        if (responseStatus.Failure()) {
            UC_ERROR("CompletionPoller WriteResponse failed, handle={}, error={}",
                     record.handle.value, responseStatus);
        }
    }
}

bool CompletionPoller::TryReleaseHandle(InflightRecord& record)
{
    const auto status = transport_.ReleaseHandle(record.handle);
    if (status.Failure()) {
        UC_ERROR("CompletionPoller ReleaseHandle failed, handle={}, error={}", record.handle.value,
                 status);
        return false;
    }
    return true;
}

bool CompletionPoller::OperationTimedOut(const InflightRecord& record, std::uint64_t nowMs) const
{
    if (nowMs < record.submit_ms) { return false; }
    return nowMs - record.submit_ms >= options_.operation_timeout_ms;
}

}  // namespace UC::DRAMPOOL
