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

#include <chrono>
#include <thread>
#include <utility>

namespace UC::DRAMPOOL {
namespace {

std::uint64_t NowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::uint64_t ExpireAtMs(std::uint64_t now_ms, std::uint64_t ttl_ms)
{
    return ttl_ms == 0 ? 0 : now_ms + ttl_ms;
}

UC::DRAMPOOL::KvOpcode RequestOpcode(const UC::DRAMPOOL::KvRequest& request)
{
    if (const auto* dump_load = dynamic_cast<const UC::DRAMPOOL::KvDumpLoadRequest*>(&request);
        dump_load != nullptr) {
        return dump_load->opcode;
    }
    if (const auto* lookup = dynamic_cast<const UC::DRAMPOOL::KvLookupRequest*>(&request);
        lookup != nullptr) {
        return lookup->opcode;
    }
    return UC::DRAMPOOL::KvOpcode::None;
}

std::uint32_t LoadFailureCode(LoadPinCode code)
{
    switch (code) {
        case LoadPinCode::NotFound: return ResultCode::kNotFound;
        case LoadPinCode::NotReady: return ResultCode::kNotReady;
        case LoadPinCode::Expired: return ResultCode::kExpired;
        case LoadPinCode::Failed:
        case LoadPinCode::Pinned: return ResultCode::kInternalError;
    }
    return ResultCode::kInternalError;
}

}  // namespace

TaskWorker::TaskWorker(TaskWorkerDeps deps, TaskWorkerConfig config)
    : deps_(deps), config_(config)
{
}

UC::Status TaskWorker::Validate() const
{
    if (deps_.request_queue == nullptr) {
        return UC::Status::InvalidParam("TaskWorker request_queue is null");
    }
    if (deps_.trans_handle_queue == nullptr) {
        return UC::Status::InvalidParam("TaskWorker trans_handle_queue is null");
    }
    if (deps_.metadata == nullptr) { return UC::Status::InvalidParam("TaskWorker metadata is null"); }
    if (deps_.buffer_manager == nullptr) {
        return UC::Status::InvalidParam("TaskWorker buffer_manager is null");
    }
    if (deps_.transport == nullptr) {
        return UC::Status::InvalidParam("TaskWorker transport is null");
    }
    if (deps_.response_writer == nullptr) {
        return UC::Status::InvalidParam("TaskWorker response_writer is null");
    }
    return UC::Status::OK();
}

void TaskWorker::Run(const std::atomic_bool& stop)
{
    if (Validate().Failure()) { return; }

    deps_.request_queue->ConsumerLoop(stop, [this](RequestPtr request) {
        const auto status = ProcessOne(std::move(request));
        (void)status;
    });
}

UC::Status TaskWorker::ProcessOne(RequestPtr request)
{
    if (!request) { return UC::Status::InvalidParam("TaskWorker got null request"); }

    switch (RequestOpcode(*request)) {
        case UC::DRAMPOOL::KvOpcode::Dump: {
            const auto* dump = dynamic_cast<const UC::DRAMPOOL::KvDumpLoadRequest*>(request.get());
            if (dump == nullptr) { return UC::Status::InvalidParam("Dump request type mismatch"); }
            return ProcessDump(*dump);
        }
        case UC::DRAMPOOL::KvOpcode::Load: {
            const auto* load = dynamic_cast<const UC::DRAMPOOL::KvDumpLoadRequest*>(request.get());
            if (load == nullptr) { return UC::Status::InvalidParam("Load request type mismatch"); }
            return ProcessLoad(*load);
        }
        case UC::DRAMPOOL::KvOpcode::Lookup: {
            const auto* lookup = dynamic_cast<const UC::DRAMPOOL::KvLookupRequest*>(request.get());
            if (lookup == nullptr) { return UC::Status::InvalidParam("Lookup request type mismatch"); }
            return ProcessLookup(*lookup);
        }
        case UC::DRAMPOOL::KvOpcode::None:
            return UC::Status::InvalidParam("TaskWorker got request with none opcode");
    }
    return UC::Status::InvalidParam("TaskWorker got request with unknown opcode");
}

UC::Status TaskWorker::ProcessDump(const UC::DRAMPOOL::KvDumpLoadRequest& request)
{
    if (request.batch_size != request.entries.size()) {
        return UC::Status::InvalidParam("Dump request batch size mismatch");
    }

    const auto now_ms = NowMs();
    std::vector<std::uint32_t> results(request.batch_size, ResultCode::kOk);
    std::vector<TransferItem> transfer_items;
    std::vector<TransportSegment> segments;
    transfer_items.reserve(request.entries.size());
    segments.reserve(request.entries.size());

    for (std::uint16_t index = 0; index < request.batch_size; ++index) {
        const auto& entry = request.entries[index];
        const Key key = entry.key;
        if (deps_.metadata->Contains(key)) {
            results[index] = ResultCode::kKeyExists;
            continue;
        }

        auto allocated = deps_.buffer_manager->Allocate(entry.len);
        if (!allocated.HasValue()) {
            results[index] = ResultCode::kNoSpace;
            break;
        }
        auto slot = std::move(allocated).Value();

        EntryCreateOptions options;
        options.key = key;
        options.buffer_handle = slot.handle;
        options.local_addr = slot.addr;
        options.len = entry.len;
        options.abs_pos = entry.idx;
        options.expire_at_ms = ExpireAtMs(now_ms, config_.default_dump_ttl_ms);
        options.class_id = slot.class_id;

        const auto reserve = deps_.metadata->ReserveDumpEntry(options);
        if (reserve.code == ReserveDumpCode::Exists) {
            deps_.buffer_manager->Free(slot.handle);
            results[index] = ResultCode::kKeyExists;
            continue;
        }
        if (reserve.code != ReserveDumpCode::Reserved) {
            deps_.buffer_manager->Free(slot.handle);
            results[index] = ResultCode::kInternalError;
            break;
        }

        TransferItem item;
        item.request_index = index;
        item.key = key;
        item.remote_addr = entry.addr;
        item.len = entry.len;
        item.buffer_handle = slot.handle;
        transfer_items.emplace_back(item);

        TransportSegment segment;
        segment.local_addr = slot.addr;
        segment.remote_addr = entry.addr;
        segment.len = entry.len;
        segments.emplace_back(segment);
    }

    if (transfer_items.empty()) {
        return deps_.response_writer->WriteResponse(UC::DRAMPOOL::KvOpcode::Dump,
                                                    request.resp_addr, results);
    }

    TransportOp op;
    op.opcode = UC::DRAMPOOL::KvOpcode::Dump;
    op.direction = TransportDirection::ReadRemoteToLocal;
    op.segments = std::move(segments);

    auto submitted = deps_.transport->SubmitAsync(op);
    if (!submitted.HasValue()) {
        RollbackDumpItems(transfer_items);
        for (const auto& item : transfer_items) {
            results[item.request_index] = ResultCode::kTransportError;
        }
        return deps_.response_writer->WriteResponse(UC::DRAMPOOL::KvOpcode::Dump,
                                                    request.resp_addr, results);
    }

    InflightRecord record;
    record.opcode = UC::DRAMPOOL::KvOpcode::Dump;
    record.handle = std::move(submitted).Value();
    record.resp_addr = request.resp_addr;
    record.batch_size = request.batch_size;
    record.results = std::move(results);
    record.transfer_items = std::move(transfer_items);
    record.submit_ms = NowMs();
    return SubmitInflight(std::move(record));
}

UC::Status TaskWorker::ProcessLoad(const UC::DRAMPOOL::KvDumpLoadRequest& request)
{
    if (request.batch_size != request.entries.size()) {
        return UC::Status::InvalidParam("Load request batch size mismatch");
    }

    const auto now_ms = NowMs();
    std::vector<std::uint32_t> results(request.batch_size, ResultCode::kOk);
    std::vector<TransferItem> transfer_items;
    std::vector<TransportSegment> segments;
    transfer_items.reserve(request.entries.size());
    segments.reserve(request.entries.size());

    for (std::uint16_t index = 0; index < request.batch_size; ++index) {
        const auto& entry = request.entries[index];
        const Key key = entry.key;
        auto pin = deps_.metadata->LookupAndPinLoad(key, now_ms);
        if (pin.code != LoadPinCode::Pinned) {
            results[index] = LoadFailureCode(pin.code);
            continue;
        }
        if (entry.len > pin.len) {
            deps_.metadata->UnpinLoad(key);
            results[index] = ResultCode::kInvalidRequest;
            continue;
        }

        TransferItem item;
        item.request_index = index;
        item.key = key;
        item.remote_addr = entry.addr;
        item.len = entry.len;
        item.buffer_handle = pin.buffer_handle;
        transfer_items.emplace_back(item);

        TransportSegment segment;
        segment.local_addr = pin.local_addr;
        segment.remote_addr = entry.addr;
        segment.len = entry.len;
        segments.emplace_back(segment);
    }

    if (transfer_items.empty()) {
        return deps_.response_writer->WriteResponse(UC::DRAMPOOL::KvOpcode::Load,
                                                    request.resp_addr, results);
    }

    TransportOp op;
    op.opcode = UC::DRAMPOOL::KvOpcode::Load;
    op.direction = TransportDirection::WriteLocalToRemote;
    op.segments = std::move(segments);

    auto submitted = deps_.transport->SubmitAsync(op);
    if (!submitted.HasValue()) {
        UnpinLoadItems(transfer_items);
        for (const auto& item : transfer_items) {
            results[item.request_index] = ResultCode::kTransportError;
        }
        return deps_.response_writer->WriteResponse(UC::DRAMPOOL::KvOpcode::Load,
                                                    request.resp_addr, results);
    }

    InflightRecord record;
    record.opcode = UC::DRAMPOOL::KvOpcode::Load;
    record.handle = std::move(submitted).Value();
    record.resp_addr = request.resp_addr;
    record.batch_size = request.batch_size;
    record.results = std::move(results);
    record.transfer_items = std::move(transfer_items);
    record.submit_ms = NowMs();
    return SubmitInflight(std::move(record));
}

UC::Status TaskWorker::ProcessLookup(const UC::DRAMPOOL::KvLookupRequest& request)
{
    if (request.batch_size != request.entries.size()) {
        return UC::Status::InvalidParam("Lookup request batch size mismatch");
    }

    const auto now_ms = NowMs();
    std::uint32_t prefix_count = 0;
    for (std::uint16_t index = 0; index < request.batch_size; ++index) {
        const Key key = request.entries[index].key;
        const auto code = deps_.metadata->LookupReady(key, now_ms);
        if (code == LookupCode::Ready) {
            ++prefix_count;
            continue;
        }
        break;
    }

    return deps_.response_writer->WriteResponse(UC::DRAMPOOL::KvOpcode::Lookup,
                                                request.resp_addr, {prefix_count});
}

void TaskWorker::RollbackDumpItems(const std::vector<TransferItem>& items)
{
    for (const auto& item : items) {
        deps_.metadata->RemoveReserved(item.key);
        deps_.buffer_manager->Free(item.buffer_handle);
    }
}

void TaskWorker::UnpinLoadItems(const std::vector<TransferItem>& items)
{
    for (const auto& item : items) { deps_.metadata->UnpinLoad(item.key); }
}

UC::Status TaskWorker::SubmitInflight(InflightRecord&& record)
{
    deps_.trans_handle_queue->Push(std::move(record));
    return UC::Status::OK();
}

}  // namespace UC::DRAMPOOL
