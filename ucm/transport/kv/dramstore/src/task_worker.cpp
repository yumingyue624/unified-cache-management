/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * DEALINGS IN THE SOFTWARE.
 * */
#include "task_worker.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>
#include "core/transport_manager.h"
#include "drampool_config.h"
#include "drampool_fake_deps.h"
#include "logger.h"

namespace UC::DRAMPOOL {
namespace {

constexpr auto kTaskWorkerIdleWait = std::chrono::microseconds(100);

std::uint64_t SteadyNowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

MetadataIndex::TimePoint LifeTimeout(std::uint64_t ttlMs)
{
    return ttlMs == 0 ? MetadataIndex::TimePoint{}
                      : std::chrono::system_clock::now() + std::chrono::milliseconds(ttlMs);
}

template <typename WireKey>
BlockId CopyBlockId(const WireKey& wireKey)
{
    static_assert(sizeof(BlockId) == sizeof(WireKey), "wire key and BlockId must have equal size");
    BlockId key{};
    std::memcpy(key.data(), wireKey.data(), key.size());
    return key;
}

std::uint32_t LoadFailureCode(LoadPinCode /*code*/)
{ return static_cast<std::uint32_t>(ResultCode::Failed); }

}  // namespace

void TaskWorker::Run(const std::atomic_bool& stop)
{
    while (true) {
        RequestTaskPtr task;
        if (runtime_.requestQueue.TryPop(task)) {
            const auto processStatus = ProcessOneRequest(std::move(task));
            if (processStatus.Failure()) {
                UC_ERROR("TaskWorker ProcessOneRequest failed: {}", processStatus);
            }
            continue;
        }

        // Stop is requested only after RequestReceiveLoop has exited, so an empty queue is drained.
        if (stop.load(std::memory_order_acquire)) { break; }
        std::this_thread::sleep_for(kTaskWorkerIdleWait);
    }
}

UC::Status TaskWorker::ProcessOneRequest(RequestTaskPtr task)
{
    if (!task || !task->request || task->peer_manager_id.empty()) {
        return UC::Status::InvalidParam("TaskWorker got an invalid request task");
    }
    const auto peerStatus = EnsurePeerReady(task->peer_manager_id);
    if (peerStatus.Failure()) { return peerStatus; }
    const auto& peerManagerId = task->peer_manager_id;
    const auto& request = task->request;
    switch (request->opcode) {
        case KvOpcode::Dump:
            return ProcessDump(*dynamic_cast<const KvDumpRequest*>(request.get()), peerManagerId);
        case KvOpcode::Load:
            return ProcessLoad(*dynamic_cast<const KvLoadRequest*>(request.get()), peerManagerId);
        case KvOpcode::Lookup:
            return ProcessLookup(*dynamic_cast<const KvLookupRequest*>(request.get()), peerManagerId);
        case KvOpcode::None: break;
    }
    return UC::Status::InvalidParam("TaskWorker got invalid opcode");
}

UC::Status TaskWorker::EnsurePeerReady(const transport::ManagerID& targetManager)
{
    auto status = runtime_.transport.Connect(transport::TransportProtocol::Hixl, targetManager);
    if (status == transport::Status::Ok) { return UC::Status::OK(); }

    status = runtime_.transport.ExchangeMetadata(targetManager);
    if (status != transport::Status::Ok) {
        return ToUcStatus(status, "TransportManager::ExchangeMetadata");
    }
    return ToUcStatus(runtime_.transport.Connect(transport::TransportProtocol::Hixl, targetManager),
                      "TransportManager::Connect");
}

UC::Status TaskWorker::ProcessDump(const KvDumpRequest& request,
                                   const transport::ManagerID& peerManagerId)
{
    const std::uint64_t ttl_ms = request.ttl != 0 ? static_cast<std::uint64_t>(request.ttl)
                                                  : g_config.defaultDumpTtlMs;
    const auto lifeTimeout = LifeTimeout(ttl_ms);
    std::vector<std::uint32_t> results(request.batch_size,
                                       static_cast<std::uint32_t>(ResultCode::Ok));
    const auto mark_remaining_failed = [&results](std::size_t first) {
        std::fill(results.begin() + first, results.end(),
                  static_cast<std::uint32_t>(ResultCode::Failed));
    };
    std::vector<TransferItem> transfer_items;
    transfer_items.reserve(request.entries.size());
    transport::Operation operation;
    operation.opcode = transport::Opcode::Read;
    operation.direct = transport::OperationDirect::RemoteDeviceHost;
    operation.target_manager = peerManagerId;
    operation.ops.reserve(request.entries.size());

    for (std::uint16_t index = 0; index < request.batch_size; ++index) {
        const auto& entry = request.entries[index];
        const BlockId key = CopyBlockId(entry.key);

        auto allocated = AllocateBuffer(runtime_, entry.len);
        if (!allocated.HasValue()) {
            UC_ERROR("Dump[{}] Allocate failed, len={}", index, entry.len);
            mark_remaining_failed(index);
            break;
        }
        auto slot = std::move(allocated).Value();

        if (slot.handle.value > std::numeric_limits<std::uint32_t>::max()) {
            UC_ERROR("Dump[{}] buffer slot id exceeds Entry::slot", index);
            (void)FreeBuffer(runtime_, slot.handle);
            mark_remaining_failed(index);
            break;
        }

        auto metadataEntry = std::make_shared<UC::DramStore::Entry>();
        metadataEntry->key = key;
        metadataEntry->slot = static_cast<std::uint32_t>(slot.handle.value);
        metadataEntry->addr = reinterpret_cast<void*>(slot.addr);
        metadataEntry->size = entry.len;
        metadataEntry->lifeTimeout = lifeTimeout;
        metadataEntry->position = entry.idx;

        const auto reserve = runtime_.metadata.ReserveDumpEntry(metadataEntry, slot.handle);
        if (reserve.code == ReserveDumpCode::Exists) {
            const auto freeStatus = FreeBuffer(runtime_, slot.handle);
            if (freeStatus.Failure()) {
                UC_ERROR("Dump[{}] Free duplicate allocation failed: {}", index, freeStatus);
            }
            results[index] = static_cast<std::uint32_t>(ResultCode::Ok);
            continue;
        }
        if (reserve.code != ReserveDumpCode::Reserved) {
            const auto freeStatus = FreeBuffer(runtime_, slot.handle);
            if (freeStatus.Failure()) {
                UC_ERROR("Dump[{}] Free failed reservation failed: {}", index, freeStatus);
            }
            UC_ERROR("Dump[{}] ReserveDumpEntry failed, code={}", index,
                     static_cast<int>(reserve.code));
            mark_remaining_failed(index);
            break;
        }

        // A RESERVED entry owns this buffer until Poller publishes or aborts it.
        transfer_items.emplace_back(TransferItem{index, key, slot.handle});
        operation.ops.emplace_back(
            transport::Segment{reinterpret_cast<void*>(slot.addr), entry.addr, entry.len});
    }

    if (transfer_items.empty()) {
        return WriteResponse(runtime_, KvOpcode::Dump, request.resp_addr, peerManagerId, results);
    }

    TransportHandle handle = transport::kInvalidTransferHandle;
    const auto submit_status = runtime_.transport.ExecuteAsync(operation, handle);
    if (submit_status != transport::Status::Ok || handle == transport::kInvalidTransferHandle) {
        UC_ERROR("Dump SubmitAsync failed, items={}", transfer_items.size());
        RollbackDumpItems(transfer_items);
        for (const auto& item : transfer_items) {
            results[item.index_in_request] = static_cast<std::uint32_t>(ResultCode::Failed);
        }
        return WriteResponse(runtime_, KvOpcode::Dump, request.resp_addr, peerManagerId, results);
    }

    InflightRecord record;
    record.opcode = KvOpcode::Dump;
    record.handle = handle;
    record.response_addr = request.resp_addr;
    record.peer_manager_id = peerManagerId;
    record.results = std::move(results);
    record.transfer_items = std::move(transfer_items);
    record.submit_ms = SteadyNowMs();
    return SubmitInflight(std::move(record));
}

UC::Status TaskWorker::ProcessLoad(const KvLoadRequest& request,
                                   const transport::ManagerID& peerManagerId)
{
    const auto now = std::chrono::system_clock::now();
    std::vector<std::uint32_t> results(request.batch_size,
                                       static_cast<std::uint32_t>(ResultCode::Ok));
    std::vector<TransferItem> transfer_items;
    transfer_items.reserve(request.entries.size());
    transport::Operation operation;
    operation.opcode = transport::Opcode::Write;
    operation.direct = transport::OperationDirect::RemoteDeviceHost;
    operation.target_manager = peerManagerId;
    operation.ops.reserve(request.entries.size());

    for (std::uint16_t index = 0; index < request.batch_size; ++index) {
        const auto& entry = request.entries[index];
        const BlockId key = CopyBlockId(entry.key);
        auto pin = runtime_.metadata.LookupAndPinLoad(key, now);
        if (pin.code != LoadPinCode::Pinned) {
            results[index] = LoadFailureCode(pin.code);
            UC_ERROR("Load[{}] LookupAndPinLoad failed, code={}", index,
                     static_cast<int>(pin.code));
            continue;
        }
        if (entry.len > pin.len) {
            const auto releaseStatus = runtime_.metadata.ReleaseLoadIo(key);
            if (releaseStatus.Failure()) {
                UC_ERROR("Load[{}] ReleaseLoadIo after len mismatch failed: {}", index,
                         releaseStatus);
            }
            UC_ERROR("Load[{}] len mismatch, entry.len={} > pin.len={}", index, entry.len, pin.len);
            results[index] = static_cast<std::uint32_t>(ResultCode::Failed);
            continue;
        }

        // The LOAD pin keeps metadata and buffer alive through async transport.
        transfer_items.emplace_back(TransferItem{index, key, pin.buffer_handle});
        operation.ops.emplace_back(
            transport::Segment{reinterpret_cast<void*>(pin.local_addr), entry.addr, entry.len});
    }

    if (transfer_items.empty()) {
        return WriteResponse(runtime_, KvOpcode::Load, request.resp_addr, peerManagerId, results);
    }

    TransportHandle handle = transport::kInvalidTransferHandle;
    const auto submit_status = runtime_.transport.ExecuteAsync(operation, handle);
    if (submit_status != transport::Status::Ok || handle == transport::kInvalidTransferHandle) {
        UC_ERROR("Load SubmitAsync failed, items={}", transfer_items.size());
        UnpinLoadItems(transfer_items);
        for (const auto& item : transfer_items) {
            results[item.index_in_request] = static_cast<std::uint32_t>(ResultCode::Failed);
        }
        return WriteResponse(runtime_, KvOpcode::Load, request.resp_addr, peerManagerId, results);
    }

    InflightRecord record;
    record.opcode = KvOpcode::Load;
    record.handle = handle;
    record.response_addr = request.resp_addr;
    record.peer_manager_id = peerManagerId;
    record.results = std::move(results);
    record.transfer_items = std::move(transfer_items);
    record.submit_ms = SteadyNowMs();
    return SubmitInflight(std::move(record));
}

UC::Status TaskWorker::ProcessLookup(const KvLookupRequest& request,
                                     const transport::ManagerID& peerManagerId)
{
    const auto now = std::chrono::system_clock::now();
    std::uint32_t prefix_count = 0;
    for (std::uint16_t index = 0; index < request.batch_size; ++index) {
        const BlockId key = CopyBlockId(request.entries[index].key);
        const auto code = runtime_.metadata.LookupReady(key, now);
        if (code == LookupCode::Ready) {
            ++prefix_count;
            continue;
        }
        break;
    }

    return WriteResponse(runtime_, KvOpcode::Lookup, request.resp_addr, peerManagerId,
                         {prefix_count});
}

void TaskWorker::RollbackDumpItems(const std::vector<TransferItem>& items)
{
    for (const auto& item : items) {
        // Remove metadata first so no index can retain a freed buffer address.
        const auto abortStatus = runtime_.metadata.AbortDump(item.key);
        if (abortStatus.Failure()) {
            UC_ERROR("RollbackDumpItems AbortDump failed: {}", abortStatus);
            continue;
        }
        const auto freeStatus = FreeBuffer(runtime_, item.buffer_handle);
        if (freeStatus.Failure()) { UC_ERROR("RollbackDumpItems Free failed: {}", freeStatus); }
    }
}

void TaskWorker::UnpinLoadItems(const std::vector<TransferItem>& items)
{
    for (const auto& item : items) {
        const auto status = runtime_.metadata.ReleaseLoadIo(item.key);
        if (status.Failure()) { UC_ERROR("UnpinLoadItems ReleaseLoadIo failed: {}", status); }
    }
}

UC::Status TaskWorker::SubmitInflight(InflightRecord&& record)
{
    // TaskWorker is the sole producer; CompletionPoller is the sole consumer.
    runtime_.transHandleQueue.Push(std::move(record));
    return UC::Status::OK();
}

}  // namespace UC::DRAMPOOL
