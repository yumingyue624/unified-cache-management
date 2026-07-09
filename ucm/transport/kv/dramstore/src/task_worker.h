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
#include <cassert>
#include <cstdint>
#include <vector>
#include "drampool_fake_deps.h"
#include "drampool_server.h"
#include "kv_protocol.h"
#include "status/status.h"

namespace UC::DRAMPOOL {

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
    TaskWorker(TaskWorkerDeps deps) : deps_(deps) {}

    void Run(const std::atomic_bool& stop);
    UC::Status ProcessOneRequest(RequestPtr request);

private:
    UC::Status ProcessDump(const KvDumpRequest& request);
    UC::Status ProcessLoad(const KvLoadRequest& request);
    UC::Status ProcessLookup(const KvLookupRequest& request);

    void RollbackDumpItems(const std::vector<TransferItem>& items);
    void UnpinLoadItems(const std::vector<TransferItem>& items);
    UC::Status SubmitInflight(InflightRecord&& record);

    TaskWorkerDeps deps_;
};

}  // namespace UC::DRAMPOOL
