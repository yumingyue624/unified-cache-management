/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#include "completion_poller.h"
#include <algorithm>
#include <chrono>
#include <utility>
#include "core/transport_manager.h"
#include "drampool_buffer.h"
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

        const auto drained = DrainNewCompletions();
        PollPendingCompletions();

        if (stop.load(std::memory_order_acquire) && drained == 0 && pending_.empty()) { break; }
    }
}

void CompletionPoller::RequestDrainAllAsFailed() noexcept
{ failAllRequested_.store(true, std::memory_order_release); }

std::size_t CompletionPoller::DrainNewCompletions()
{
    std::size_t drained = 0;
    while (drained < g_config.pollerDrainBudget && pending_.size() < g_config.pollerMaxPending) {
        CompletionRecord record;
        if (!runtime_.completionQueue.TryPop(record)) { break; }
        pending_.emplace_back(std::move(record));
        ++drained;
    }
    return drained;
}

void CompletionPoller::PollPendingCompletions()
{
    const std::size_t scanCount =
        std::min(static_cast<std::size_t>(g_config.pollerScanBudget), pending_.size());
    auto iter = pending_.begin();

    // Scan only this head snapshot. Erase never pulls extra work into this round.
    for (std::size_t scanned = 0; scanned < scanCount; ++scanned) {
        switch (iter->stage) {
        case CompletionStage::DataTransfer:
            if (!ProcessDataTransfer(*iter)) {
                ++iter;
                break;
            }
            // Data settlement moves the record to ResponseReady. Submit its response
            // in this scan instead of deferring it to the next poll round.
            [[fallthrough]];
        case CompletionStage::ResponseReady: {
            const auto status = SubmitResponse(*iter);
            if (status.Failure()) {
                UC_ERROR("CompletionPoller SubmitResponse failed, opcode={}, error={}",
                         static_cast<int>(iter->opcode), status);
                iter = pending_.erase(iter);
            } else {
                ++iter;
            }
            break;
        }
        case CompletionStage::ResponseTransfer:
            if (ProcessResponseTransfer(*iter)) {
                iter = pending_.erase(iter);
            } else {
                ++iter;
            }
            break;
        default:
            UC_ERROR("CompletionPoller got invalid completion stage={}",
                     static_cast<int>(iter->stage));
            iter = pending_.erase(iter);
            break;
        }
    }
}

bool CompletionPoller::ProcessDataTransfer(CompletionRecord& record)
{
    transport::TransferStatus transportStatus = transport::TransferStatus::Failed;
    const auto queryStatus = runtime_.transport.GetStatus(record.data_handle, transportStatus);
    if (queryStatus != transport::Status::Ok) {
        // GetStatus removes failed handles, so an API failure is also terminal.
        UC_ERROR("CompletionPoller data GetStatus failed, handle={}", record.data_handle);
        SettleDataTransfer(record, transport::TransferStatus::Failed);
        record.data_handle = transport::kInvalidTransferHandle;
        record.stage = CompletionStage::ResponseReady;
        return true;
    }

    if (transportStatus == transport::TransferStatus::Waiting) {
        if (record.phase == InflightPhase::Polling &&
            failAllRequested_.load(std::memory_order_acquire)) {
            record.phase = InflightPhase::FailurePending;
        }
        if (record.phase == InflightPhase::Polling &&
            OperationTimedOut(record, SteadyNowMs())) {
            record.phase = InflightPhase::FailurePending;
        }
        return false;
    }

    if (record.phase == InflightPhase::FailurePending) {
        transportStatus = transport::TransferStatus::Failed;
    }
    // A terminal GetStatus releases the data handle before business state is settled.
    SettleDataTransfer(record, transportStatus);
    record.data_handle = transport::kInvalidTransferHandle;
    record.stage = CompletionStage::ResponseReady;
    return true;
}

UC::Status CompletionPoller::SubmitResponse(CompletionRecord& record)
{
    if (record.peer_manager_id.empty()) {
        return UC::Status::InvalidParam("response peer_manager_id is empty");
    }

    const auto len = static_cast<std::uint32_t>(record.results.size() * sizeof(std::uint32_t));
    auto allocated = AllocateBuffer(runtime_, len);
    if (!allocated.HasValue()) { return allocated.Error(); }
    auto slot = std::move(allocated).Value();

    const auto protocolStatus = runtime_.protocol.PackResponse(
        reinterpret_cast<void*>(slot.addr), record.opcode, KvResponse{record.results});
    if (!protocolStatus.ok()) {
        const auto freeStatus = FreeBuffer(runtime_, slot.handle);
        if (freeStatus.Failure()) {
            UC_ERROR("CompletionPoller Free response buffer after pack failure failed: {}",
                     freeStatus);
        }
        return UC::Status::Error(protocolStatus.message);
    }

    transport::Operation operation;
    operation.opcode = transport::Opcode::Write;
    operation.direct = transport::OperationDirect::RemoteDeviceHost;
    operation.target_manager = record.peer_manager_id;
    operation.ops.emplace_back(
        transport::Segment{reinterpret_cast<void*>(slot.addr), record.response_addr, len});

    TransportHandle handle = transport::kInvalidTransferHandle;
    const auto submitStatus = runtime_.transport.ExecuteAsync(operation, handle);
    if (submitStatus != transport::Status::Ok ||
        handle == transport::kInvalidTransferHandle) {
        const auto freeStatus = FreeBuffer(runtime_, slot.handle);
        if (freeStatus.Failure()) {
            UC_ERROR("CompletionPoller Free response buffer after submit failure failed: {}",
                     freeStatus);
        }
        if (submitStatus != transport::Status::Ok) {
            return ToUcStatus(submitStatus, "ExecuteAsync response");
        }
        return UC::Status::Error("ExecuteAsync response returned an invalid handle");
    }

    // ExecuteAsync now owns access to the bytes; retain the buffer until handle terminal.
    record.response_handle = handle;
    record.response_buffer = slot.handle;
    record.results.clear();
    record.stage = CompletionStage::ResponseTransfer;
    return UC::Status::OK();
}

bool CompletionPoller::ProcessResponseTransfer(CompletionRecord& record)
{
    transport::TransferStatus transportStatus = transport::TransferStatus::Failed;
    const auto queryStatus =
        runtime_.transport.GetStatus(record.response_handle, transportStatus);
    if (queryStatus != transport::Status::Ok) {
        // GetStatus removes failed handles, so the response source buffer is no longer in use.
        UC_ERROR("CompletionPoller response GetStatus failed, handle={}",
                 record.response_handle);
    } else if (transportStatus == transport::TransferStatus::Waiting) {
        return false;
    } else if (!IsSuccessful(transportStatus)) {
        UC_ERROR("CompletionPoller response transfer failed, handle={}",
                 record.response_handle);
    }

    const auto freeStatus = FreeBuffer(runtime_, record.response_buffer);
    if (freeStatus.Failure()) {
        UC_ERROR("CompletionPoller Free response buffer failed, handle={}, error={}",
                 record.response_handle, freeStatus);
    }
    return true;
}

void CompletionPoller::SettleDataTransfer(CompletionRecord& record,
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
                             record.data_handle, status);
                }
            } else {
                const auto abortStatus = runtime_.metadata.AbortDump(item.key);
                if (abortStatus.Success()) {
                    const auto freeStatus = FreeBuffer(runtime_, item.buffer_handle);
                    if (freeStatus.Failure()) {
                        UC_ERROR(
                            "CompletionPoller Free failed after DUMP abort, handle={}, error={}",
                            record.data_handle, freeStatus);
                    }
                } else {
                    UC_ERROR("CompletionPoller AbortDump failed, handle={}, error={}",
                             record.data_handle, abortStatus);
                }
            }
        } else if (record.opcode == KvOpcode::Load) {
            const auto releaseStatus = runtime_.metadata.ReleaseLoadIo(item.key);
            if (releaseStatus.Failure()) {
                UC_ERROR("CompletionPoller ReleaseLoadIo failed, handle={}, error={}",
                         record.data_handle, releaseStatus);
            } else if (IsSuccessful(terminalStatus)) {
                result = ResultCode::Ok;
            }
        }

        if (item.index_in_request >= record.results.size()) {
            UC_ERROR("CompletionPoller result index out of range, handle={}, index={}",
                     record.data_handle, item.index_in_request);
            continue;
        }
        record.results[item.index_in_request] = static_cast<std::uint32_t>(result);
    }
    record.transfer_items.clear();
}

bool CompletionPoller::OperationTimedOut(const CompletionRecord& record,
                                         std::uint64_t nowMs) const
{
    if (nowMs < record.submit_ms) { return false; }
    return nowMs - record.submit_ms >= g_config.opTimeoutMs;
}

}  // namespace UC::DRAMPOOL
