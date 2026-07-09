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
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "drampool_config.h"
#include "status/status.h"

namespace UC::DRAMPOOL {

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

    mutable std::mutex lifecycleMutex_;
    std::vector<std::string> lifecycleEvents_;
};

}  // namespace UC::DRAMPOOL
