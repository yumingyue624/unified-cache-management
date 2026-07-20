/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#include "completion_poller.h"
#include <chrono>
#include <limits>
#include <utility>
#include "core/transport_manager.h"
#include "drampool_config.h"
#include "logger/logger.h"
#include "metadata.h"

namespace UC::DramPool {
namespace {

std::uint64_t SteadyNowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool IsSuccessful(transport::TransferStatus status)
{
    return status == transport::TransferStatus::Completed;
}

}  // namespace

void CompletionPoller::Run(const std::atomic_bool& stop)
{
    while (true) {
        if (stop.load(std::memory_order_acquire)) {
            // The formal transport has no per-transfer Cancel; drain each handle safely.
            failAllRequested_.store(true, std::memory_order_release);
        }

        const auto filled = FillPendingWindow();
        PollPendingCompletions();

        if (stop.load(std::memory_order_acquire) && filled == 0 && pending_.empty()) { break; }
    }
}

void CompletionPoller::RequestDrainAllAsFailed() noexcept
{
    failAllRequested_.store(true, std::memory_order_release);
}

std::size_t CompletionPoller::FillPendingWindow()
{
    std::size_t filled = 0;
    while (pending_.size() < g_config.pollerPendingDepth) {
        CompletionRecord record;
        if (!runtime_.completionQueue.TryPop(record)) { break; }
        pending_.emplace_back(std::move(record));
        ++filled;
    }
    return filled;
}

void CompletionPoller::PollPendingCompletions()
{
    const std::size_t scanCount = pending_.size();
    auto iter = pending_.begin();

    // Scan the whole pending snapshot. Completed records free slots that are refilled
    // from completionQueue at the beginning of the next round.
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
        if (record.phase == InflightPhase::Polling && OperationTimedOut(record, SteadyNowMs())) {
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

Status CompletionPoller::SubmitResponse(CompletionRecord& record)
{
    if (record.peer_one_sided_id.empty()) {
        return Status::InvalidParam("response peer_one_sided_id is empty");
    }

    const auto packedSize =
        runtime_.protocol.GetPackedResponseSize(record.opcode, record.results.size());
    if (packedSize == 0 || packedSize > std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidParam("invalid packed response size");
    }
    const auto len = static_cast<std::uint32_t>(packedSize);
    auto allocateStatus = AcquireBufferAtLeast(runtime_.bufferManager, len, record.response_buffer);
    if (allocateStatus.Failure()) { return allocateStatus; }
    const auto& responseBuffer = record.response_buffer.Get();

    const auto protocolStatus = runtime_.protocol.PackResponse(responseBuffer.addr, record.opcode,
                                                               KvResponse{record.results});
    if (protocolStatus.Failure()) { return protocolStatus; }

    transport::Operation operation;
    operation.opcode = transport::Opcode::Write;
    operation.direct = transport::OperationDirect::RemoteDeviceHost;
    operation.target_manager = record.peer_one_sided_id;
    operation.ops.emplace_back(transport::Segment{responseBuffer.addr, record.response_addr, len});

    TransportHandle handle = transport::kInvalidTransferHandle;
    const auto submitStatus = runtime_.transport.ExecuteAsync(operation, handle);
    if (submitStatus != transport::Status::Ok || handle == transport::kInvalidTransferHandle) {
        if (submitStatus != transport::Status::Ok) {
            return ToUcStatus(submitStatus, "ExecuteAsync response");
        }
        return Status::Error("ExecuteAsync response returned an invalid handle");
    }

    // ExecuteAsync now owns access to the bytes; retain the buffer until handle terminal.
    record.response_handle = handle;
    record.results.clear();
    record.stage = CompletionStage::ResponseTransfer;
    return Status::OK();
}

bool CompletionPoller::ProcessResponseTransfer(CompletionRecord& record)
{
    transport::TransferStatus transportStatus = transport::TransferStatus::Failed;
    const auto queryStatus = runtime_.transport.GetStatus(record.response_handle, transportStatus);
    if (queryStatus != transport::Status::Ok) {
        // GetStatus removes failed handles, so the response source buffer is no longer in use.
        UC_ERROR("CompletionPoller response GetStatus failed, handle={}", record.response_handle);
    } else if (transportStatus == transport::TransferStatus::Waiting) {
        return false;
    } else if (!IsSuccessful(transportStatus)) {
        UC_ERROR("CompletionPoller response transfer failed, handle={}", record.response_handle);
    }

    const auto releaseStatus = record.response_buffer.Reset();
    if (releaseStatus.Failure()) {
        UC_ERROR("CompletionPoller release response buffer failed, handle={}, error={}",
                 record.response_handle, releaseStatus);
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
                const auto status = runtime_.metadata.StoreEnd(item.key);
                if (status.Success()) {
                    result = ResultCode::Ok;
                } else {
                    UC_ERROR("CompletionPoller StoreEnd failed, handle={}, error={}",
                             record.data_handle, status);
                    const auto abortStatus = runtime_.metadata.Delete(item.key);
                    if (abortStatus.Failure()) {
                        UC_ERROR(
                            "CompletionPoller Delete failed after StoreEnd error, handle={}, "
                            "error={}",
                            record.data_handle, abortStatus);
                    }
                }
            } else {
                const auto abortStatus = runtime_.metadata.Delete(item.key);
                if (abortStatus.Failure()) {
                    UC_ERROR("CompletionPoller Delete reserved DUMP failed, handle={}, error={}",
                             record.data_handle, abortStatus);
                }
            }
        } else if (record.opcode == KvOpcode::Load) {
            const auto releaseStatus = runtime_.metadata.LoadEnd(item.key);
            if (releaseStatus.Failure()) {
                UC_ERROR("CompletionPoller LoadEnd failed, handle={}, error={}", record.data_handle,
                         releaseStatus);
            } else if (IsSuccessful(terminalStatus)) {
                result = ResultCode::Ok;
            }
        }

        if (item.index_in_request >= record.results.size()) {
            UC_ERROR("CompletionPoller result index out of range, handle={}, index={}",
                     record.data_handle, item.index_in_request);
            continue;
        }
        record.results[item.index_in_request] = static_cast<std::uint8_t>(result);
    }
    record.transfer_items.clear();
}

bool CompletionPoller::OperationTimedOut(const CompletionRecord& record, std::uint64_t nowMs) const
{
    if (nowMs < record.submit_ms) { return false; }
    return nowMs - record.submit_ms >= g_config.opTimeoutMs;
}

}  // namespace UC::DramPool
