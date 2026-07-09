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
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "drampool_fake_deps.h"
#include "kv_protocol.h"
#include "status/status.h"
#include "template/spsc_ring_queue.h"

namespace UC::DRAMPOOL {

using RequestPtr = std::unique_ptr<UC::DRAMPOOL::KvRequest>;
using RequestQueue = UC::SpscRingQueue<RequestPtr>;

struct TransferItem {
    std::uint16_t request_index{0};
    Key key{};
    std::uint64_t remote_addr{0};
    std::uint32_t len{0};
    BufferHandle buffer_handle;
};

struct InflightRecord {
    UC::DRAMPOOL::KvOpcode opcode{UC::DRAMPOOL::KvOpcode::None};
    TransportHandle handle;

    std::uint64_t resp_addr{0};
    std::uint16_t batch_size{0};

    std::vector<std::uint32_t> results;
    std::vector<TransferItem> transfer_items;

    std::uint64_t submit_ms{0};
};

using TransHandleQueue = UC::SpscRingQueue<InflightRecord>;

namespace ResultCode {
constexpr std::uint32_t kOk = 0;
constexpr std::uint32_t kKeyExists = 1;
constexpr std::uint32_t kNotFound = 2;
constexpr std::uint32_t kNotReady = 3;
constexpr std::uint32_t kExpired = 4;
constexpr std::uint32_t kNoSpace = 5;
constexpr std::uint32_t kTransportError = 6;
constexpr std::uint32_t kInvalidRequest = 7;
constexpr std::uint32_t kInternalError = 8;
}  // namespace ResultCode

struct TaskWorkerConfig {
    std::uint64_t default_dump_ttl_ms{0};
};

struct TaskWorkerDeps {
    RequestQueue* request_queue{nullptr};
    TransHandleQueue* trans_handle_queue{nullptr};
    MetadataIndex* metadata{nullptr};
    BufferManager* buffer_manager{nullptr};
    TransportManager* transport{nullptr};
    ResponseWriter* response_writer{nullptr};
};

class TaskWorker final {
public:
    TaskWorker(TaskWorkerDeps deps, TaskWorkerConfig config = {});

    UC::Status Validate() const;
    void Run(const std::atomic_bool& stop);
    UC::Status ProcessOne(RequestPtr request);

private:
    UC::Status ProcessDump(const UC::DRAMPOOL::KvDumpLoadRequest& request);
    UC::Status ProcessLoad(const UC::DRAMPOOL::KvDumpLoadRequest& request);
    UC::Status ProcessLookup(const UC::DRAMPOOL::KvLookupRequest& request);

    void RollbackDumpItems(const std::vector<TransferItem>& items);
    void UnpinLoadItems(const std::vector<TransferItem>& items);
    UC::Status SubmitInflight(InflightRecord&& record);

    TaskWorkerDeps deps_;
    TaskWorkerConfig config_;
};

}  // namespace UC::DRAMPOOL
