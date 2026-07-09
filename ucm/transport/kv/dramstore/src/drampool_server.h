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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "drampool_config.h"
#include "drampool_fake_deps.h"
#include "kv_protocol.h"
#include "status/status.h"
#include "template/spsc_ring_queue.h"

namespace UC::DRAMPOOL {

using BlockId = UC::Detail::BlockId;
using RequestPtr = std::unique_ptr<KvRequest>;
using RequestQueue = UC::SpscRingQueue<RequestPtr>;

struct BufferHandle {
    ScatterGatherEntry sge{};
    std::uint16_t slab_id{0};
};

struct TransferItem {
    std::uint16_t request_index{0};
    BlockId key{};
    std::uint64_t remote_addr{0};
    std::uint32_t len{0};
    BufferHandle buffer_handle;
};

struct InflightRecord {
    KvOpcode opcode{KvOpcode::None};
    TransportHandle handle;

    std::uint64_t resp_addr{0};
    std::uint16_t batch_size{0};

    std::vector<std::uint32_t> results;
    std::vector<TransferItem> transfer_items;

    std::uint64_t submit_ms{0};
};

using TransHandleQueue = UC::SpscRingQueue<InflightRecord>;

enum class ResultCode : std::uint32_t {
    Ok = 0,
    Failed = 1,
};

class TaskWorker;

class DramPoolServer {
public:
    DramPoolServer() = default;
    ~DramPoolServer();

    DramPoolServer(const DramPoolServer&) = delete;
    DramPoolServer& operator=(const DramPoolServer&) = delete;

    UC::Status Init(const DramPoolConfig& config);
    UC::Status Start();
    void Stop();

    bool IsServiceReady() const noexcept;
    std::vector<std::string> LifecycleEvents() const;

private:
    UC::Status InitDataTransportManager();
    UC::Status InstallDataTransport();
    UC::Status InitBufferMgr();
    UC::Status RegisterBufferMemory();
    UC::Status InitMetadataIndex();
    UC::Status InitProtocol();
    UC::Status InitQueues();

    UC::Status StartCompletionPoller();
    UC::Status StartTaskWorker();
    UC::Status StartGCThread();
    UC::Status StartRequestChannelAndReceiver();

    void SetServiceReady(bool ready);
    void StopReceiver();
    void StopTaskWorker();
    void CancelInflightTransports();
    void StopCompletionPoller();
    void StopGCThread();
    void UnregisterBufferMemory();
    void DestroyMetadataIndex();

    void ReceiverLoop();
    void TaskWorkerLoop();
    void CompletionPollerLoop();
    void GCThreadLoop();
    void RecordLifecycleEvent(const std::string& event);

    DramPoolConfig config_;
    std::atomic_bool initialized_{false};
    std::atomic_bool started_{false};
    std::atomic_bool serviceReady_{false};

    std::atomic_bool receiverStop_{true};
    std::atomic_bool taskWorkerStop_{true};
    std::atomic_bool completionPollerStop_{true};
    std::atomic_bool gcThreadStop_{true};

    std::thread receiverThread_;
    std::thread taskWorkerThread_;
    std::thread completionPollerThread_;
    std::thread gcThread_;

    RequestQueue requestQueue_;
    TransHandleQueue transHandleQueue_;
    std::unique_ptr<TaskWorker> taskWorker_;

    mutable std::mutex lifecycleMutex_;
    std::vector<std::string> lifecycleEvents_;
};

}  // namespace UC::DRAMPOOL
