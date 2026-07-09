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

KvOpcode RequestOpcode(const KvRequest& request)
{
    if (const auto* dump = dynamic_cast<const KvDumpRequest*>(&request); dump != nullptr) {
        return dump->opcode;
    }
    if (const auto* load = dynamic_cast<const KvLoadRequest*>(&request); load != nullptr) {
        return load->opcode;
    }
    if (const auto* lookup = dynamic_cast<const KvLookupRequest*>(&request); lookup != nullptr) {
        return lookup->opcode;
    }
    return KvOpcode::None;
}

constexpr std::uint32_t ToUint32(ResultCode code) { return static_cast<std::uint32_t>(code); }

std::uint32_t LoadFailureCode(LoadPinCode /*code*/) { return ToUint32(ResultCode::Failed); }

}  // namespace

void TaskWorker::Run(const std::atomic_bool& stop)
{
    deps_.request_queue->ConsumerLoop(stop, [this](RequestPtr request) {
        const auto status = ProcessOneRequest(std::move(request));
        if (status.Failure()) { UC_ERROR("TaskWorker ProcessOneRequest failed: {}", status); }
    });
}

UC::Status TaskWorker::ProcessOneRequest(RequestPtr request)
{
    if (!request) { return UC::Status::InvalidParam("TaskWorker got null request"); }

    switch (RequestOpcode(*request)) {
        case KvOpcode::Dump: {
            const auto* dump = dynamic_cast<const KvDumpRequest*>(request.get());
            if (dump == nullptr) { return UC::Status::InvalidParam("Dump request type mismatch"); }
            return ProcessDump(*dump);
        }
        case KvOpcode::Load: {
            const auto* load = dynamic_cast<const KvLoadRequest*>(request.get());
            if (load == nullptr) { return UC::Status::InvalidParam("Load request type mismatch"); }
            return ProcessLoad(*load);
        }
        case KvOpcode::Lookup: {
            const auto* lookup = dynamic_cast<const KvLookupRequest*>(request.get());
            if (lookup == nullptr) {
                return UC::Status::InvalidParam("Lookup request type mismatch");
            }
            return ProcessLookup(*lookup);
        }
        case KvOpcode::None:
            return UC::Status::InvalidParam("TaskWorker got request with none opcode");
    }
    return UC::Status::InvalidParam("TaskWorker got request with unknown opcode");
}

UC::Status TaskWorker::ProcessDump(const KvDumpRequest& request)
{
    if (request.batch_size != request.entries.size()) {
        return UC::Status::InvalidParam("Dump request batch size mismatch");
    }

    const auto now_ms = NowMs();
    std::vector<std::uint32_t> results(request.batch_size, ToUint32(ResultCode::Ok));
    std::vector<TransferItem> transfer_items;
    std::vector<TransportSegment> segments;
    transfer_items.reserve(request.entries.size());
    segments.reserve(request.entries.size());

    for (std::uint16_t index = 0; index < request.batch_size; ++index) {
        const auto& entry = request.entries[index];
        const BlockId& key = reinterpret_cast<const BlockId&>(entry.key);
        if (deps_.metadata->Contains(key)) { continue; }

        auto allocated = deps_.buffer_manager->Allocate(entry.len);
        if (!allocated.HasValue()) {
            results[index] = ToUint32(ResultCode::Failed);
            UC_ERROR("Dump[{}] Allocate failed, len={}", index, entry.len);
            break;
        }
        auto slot = std::move(allocated).Value();

        EntryCreateOptions options;
        options.key = key;
        options.buffer_handle = slot.handle;
        options.local_addr = slot.addr;
        options.len = entry.len;
        options.abs_pos = entry.idx;
        const auto ttl = entry.ttl != 0 ? entry.ttl : g_drampool_config.defaultDumpTtlMs;
        options.expire_at_ms = ExpireAtMs(now_ms, ttl);
        options.class_id = slot.class_id;

        const auto reserve = deps_.metadata->ReserveDumpEntry(options);
        if (reserve.code == ReserveDumpCode::Exists) {
            deps_.buffer_manager->Free(slot.handle);
            continue;
        }
        if (reserve.code != ReserveDumpCode::Reserved) {
            deps_.buffer_manager->Free(slot.handle);
            results[index] = ToUint32(ResultCode::Failed);
            UC_ERROR("Dump[{}] ReserveDumpEntry failed, code={}", index,
                     static_cast<int>(reserve.code));
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
        return deps_.response_writer->WriteResponse(KvOpcode::Dump, request.resp_addr, results);
    }

    TransportOp op;
    op.opcode = KvOpcode::Dump;
    op.direction = TransportDirection::ReadRemoteToLocal;
    op.segments = std::move(segments);

    auto submitted = deps_.transport->SubmitAsync(op);
    if (!submitted.HasValue()) {
        UC_ERROR("Dump SubmitAsync failed, items={}", transfer_items.size());
        RollbackDumpItems(transfer_items);
        for (const auto& item : transfer_items) {
            results[item.request_index] = ToUint32(ResultCode::Failed);
        }
        return deps_.response_writer->WriteResponse(KvOpcode::Dump, request.resp_addr, results);
    }

    InflightRecord record;
    record.opcode = KvOpcode::Dump;
    record.handle = std::move(submitted).Value();
    record.resp_addr = request.resp_addr;
    record.batch_size = request.batch_size;
    record.results = std::move(results);
    record.transfer_items = std::move(transfer_items);
    record.submit_ms = NowMs();
    return SubmitInflight(std::move(record));
}

UC::Status TaskWorker::ProcessLoad(const KvLoadRequest& request)
{
    if (request.batch_size != request.entries.size()) {
        return UC::Status::InvalidParam("Load request batch size mismatch");
    }

    const auto now_ms = NowMs();
    std::vector<std::uint32_t> results(request.batch_size, ToUint32(ResultCode::Ok));
    std::vector<TransferItem> transfer_items;
    std::vector<TransportSegment> segments;
    transfer_items.reserve(request.entries.size());
    segments.reserve(request.entries.size());

    for (std::uint16_t index = 0; index < request.batch_size; ++index) {
        const auto& entry = request.entries[index];
        const BlockId& key = reinterpret_cast<const BlockId&>(entry.key);
        auto pin = deps_.metadata->LookupAndPinLoad(key, now_ms);
        if (pin.code != LoadPinCode::Pinned) {
            results[index] = LoadFailureCode(pin.code);
            UC_ERROR("Load[{}] LookupAndPinLoad failed, code={}", index,
                     static_cast<int>(pin.code));
            continue;
        }
        if (entry.len > pin.len) {
            deps_.metadata->UnpinLoad(key);
            results[index] = ToUint32(ResultCode::Failed);
            UC_ERROR("Load[{}] len mismatch, entry.len={} > pin.len={}", index, entry.len, pin.len);
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
        return deps_.response_writer->WriteResponse(KvOpcode::Load, request.resp_addr, results);
    }

    TransportOp op;
    op.opcode = KvOpcode::Load;
    op.direction = TransportDirection::WriteLocalToRemote;
    op.segments = std::move(segments);

    auto submitted = deps_.transport->SubmitAsync(op);
    if (!submitted.HasValue()) {
        UC_ERROR("Load SubmitAsync failed, items={}", transfer_items.size());
        UnpinLoadItems(transfer_items);
        for (const auto& item : transfer_items) {
            results[item.request_index] = ToUint32(ResultCode::Failed);
        }
        return deps_.response_writer->WriteResponse(KvOpcode::Load, request.resp_addr, results);
    }

    InflightRecord record;
    record.opcode = KvOpcode::Load;
    record.handle = std::move(submitted).Value();
    record.resp_addr = request.resp_addr;
    record.batch_size = request.batch_size;
    record.results = std::move(results);
    record.transfer_items = std::move(transfer_items);
    record.submit_ms = NowMs();
    return SubmitInflight(std::move(record));
}

UC::Status TaskWorker::ProcessLookup(const KvLookupRequest& request)
{
    if (request.batch_size != request.entries.size()) {
        return UC::Status::InvalidParam("Lookup request batch size mismatch");
    }

    const auto now_ms = NowMs();
    std::uint32_t prefix_count = 0;
    for (std::uint16_t index = 0; index < request.batch_size; ++index) {
        const BlockId& key = reinterpret_cast<const BlockId&>(request.entries[index].key);
        const auto code = deps_.metadata->LookupReady(key, now_ms);
        if (code == LookupCode::Ready) {
            ++prefix_count;
            continue;
        }
        break;
    }

    return deps_.response_writer->WriteResponse(KvOpcode::Lookup, request.resp_addr,
                                                {prefix_count});
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
