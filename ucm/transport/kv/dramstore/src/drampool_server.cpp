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
#include "drampool_server.h"
#include <chrono>
#include <exception>
#include <utility>
#include "logger.h"
#include "task_worker.h"

namespace UC::DRAMPOOL {
DramPoolServer::DramPoolServer() = default;

DramPoolServer::~DramPoolServer() { Stop(); }

UC::Status DramPoolServer::Init(const DramPoolConfig& config)
{
    std::lock_guard<std::mutex> controlGuard(controlMutex_);
    if (state_ != ServerState::New) {
        return UC::Status::InvalidParam(
            "DramPoolServer cannot be initialized in its current state");
    }
    auto status = ValidateDramPoolConfig(config);
    if (status.Failure()) { return status; }

    config_ = config;
    g_drampool_config = config;

    try {
        status = InitDataTransportManager();
        if (status.Failure()) {
            ResetInitializedComponents();
            return status;
        }
        status = InstallDataTransport();
        if (status.Failure()) {
            ResetInitializedComponents();
            return status;
        }
        status = InitBufferMgr();
        if (status.Failure()) {
            ResetInitializedComponents();
            return status;
        }
        status = RegisterBufferMemory();
        if (status.Failure()) {
            ResetInitializedComponents();
            return status;
        }
        status = InitMetadataIndex();
        if (status.Failure()) {
            ResetInitializedComponents();
            return status;
        }
        status = InitProtocol();
        if (status.Failure()) {
            ResetInitializedComponents();
            return status;
        }
        status = InitQueues();
        if (status.Failure()) {
            ResetInitializedComponents();
            return status;
        }
    } catch (const std::exception& error) {
        ResetInitializedComponents();
        return UC::Status::Error(std::string{"DramPoolServer initialization failed: "} +
                                 error.what());
    }

    g_services.request_queue = &requestQueue_;
    g_services.trans_handle_queue = &transHandleQueue_;
    g_services.metadata = metadataIndex_.get();
    g_services.buffer_managers.clear();
    for (auto& bm : bufferManagers_) {
        g_services.buffer_managers.push_back(bm.get());
    }
    g_services.transport = transportManager_.get();

    state_ = ServerState::Initialized;
    return UC::Status::OK();
}

UC::Status DramPoolServer::Start()
{
    std::lock_guard<std::mutex> controlGuard(controlMutex_);
    if (state_ != ServerState::Initialized) {
        return UC::Status::InvalidParam("DramPoolServer is not ready to start");
    }
    state_ = ServerState::Starting;

    // Start internal threads before the receiver makes the service externally visible.
    auto status = StartCompletionPoller();
    if (status.Failure()) {
        StopLocked();
        return status;
    }
    status = StartTaskWorker();
    if (status.Failure()) {
        StopLocked();
        return status;
    }
    status = StartGCThread();
    if (status.Failure()) {
        StopLocked();
        return status;
    }
    status = StartRequestChannelAndReceiver();
    if (status.Failure()) {
        StopLocked();
        return status;
    }
    SetServiceReady(true);
    state_ = ServerState::Ready;
    return UC::Status::OK();
}

void DramPoolServer::Stop()
{
    std::lock_guard<std::mutex> controlGuard(controlMutex_);
    StopLocked();
}

void DramPoolServer::StopLocked()
{
    if (state_ == ServerState::New || state_ == ServerState::Stopped) { return; }
    state_ = ServerState::Stopping;
    // Close ingress first, drain accepted work, then destroy shared dependencies.
    SetServiceReady(false);
    StopReceiver();
    StopTaskWorker();
    CancelInflightTransports();
    StopCompletionPoller();
    StopGCThread();
    UnregisterBufferMemory();
    DestroyMetadataIndex();
    ResetInitializedComponents();
    state_ = ServerState::Stopped;
}

bool DramPoolServer::IsServiceReady() const noexcept
{ return serviceReady_.load(std::memory_order_acquire); }

std::vector<std::string> DramPoolServer::LifecycleEvents() const
{
    std::lock_guard<std::mutex> guard(lifecycleMutex_);
    return lifecycleEvents_;
}

UC::Status DramPoolServer::InitDataTransportManager()
{
    transportManager_ = std::make_unique<FakeTransportManager>();
    RecordLifecycleEvent("InitDataTransportManager");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InstallDataTransport()
{
    if (!transportManager_) {
        return UC::Status::InvalidParam("TransportManager is not initialized");
    }
    RecordLifecycleEvent("InstallDataTransport");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InitBufferMgr()
{
    for (const auto blockSize : config_.poolBlockSizes) {
        bufferManagers_.push_back(std::make_unique<FakeBufferManager>(
            static_cast<std::uint32_t>(blockSize)));
    }
    RecordLifecycleEvent("InitBufferMgr");
    return UC::Status::OK();
}

UC::Status DramPoolServer::RegisterBufferMemory()
{
    if (bufferManagers_.empty() || !transportManager_) {
        return UC::Status::InvalidParam("buffer registration dependencies are not initialized");
    }
    RecordLifecycleEvent("RegisterBufferMemory");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InitMetadataIndex()
{
    metadataIndex_ = std::make_unique<FakeMetadataIndex>();
    RecordLifecycleEvent("InitMetadataIndex");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InitProtocol()
{
    protocolManager_ = std::make_unique<ProtocolManager>();
    g_services.protocol_mgr = protocolManager_.get();
    RecordLifecycleEvent("InitProtocol");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InitQueues()
{
    requestQueue_.Setup(config_.requestQueueDepth);
    transHandleQueue_.Setup(config_.handleQueueDepth);
    RecordLifecycleEvent("InitQueues");
    return UC::Status::OK();
}

UC::Status DramPoolServer::StartCompletionPoller()
{
    try {
        completionPoller_ = std::make_unique<CompletionPoller>();
        completionPollerStop_.store(false, std::memory_order_release);
        completionPollerThread_ = std::thread(&DramPoolServer::CompletionPollerLoop, this);
        RecordLifecycleEvent("StartCompletionPoller");
    } catch (const std::exception& e) {
        completionPoller_.reset();
        return UC::Status::Error(std::string{"failed to start CompletionPoller: "} + e.what());
    }
    return UC::Status::OK();
}

UC::Status DramPoolServer::StartTaskWorker()
{
    try {
        taskWorker_ = std::make_unique<TaskWorker>();
        taskWorkerStop_.store(false, std::memory_order_release);
        taskWorkerThread_ = std::thread(&DramPoolServer::TaskWorkerLoop, this);
        RecordLifecycleEvent("StartTaskWorker");
    } catch (const std::exception& e) {
        taskWorker_.reset();
        return UC::Status::Error(std::string{"failed to start TaskWorker: "} + e.what());
    }
    return UC::Status::OK();
}

UC::Status DramPoolServer::StartGCThread()
{
    if (!config_.gcEnabled) { return UC::Status::OK(); }
    try {
        gcThreadStop_.store(false, std::memory_order_release);
        gcThread_ = std::thread(&DramPoolServer::GCThreadLoop, this);
        RecordLifecycleEvent("StartGCThread");
    } catch (const std::exception& e) {
        return UC::Status::Error(std::string{"failed to start GCThread: "} + e.what());
    }
    return UC::Status::OK();
}

UC::Status DramPoolServer::StartRequestChannelAndReceiver()
{
    try {
        receiverStop_.store(false, std::memory_order_release);
        receiverThread_ = std::thread(&DramPoolServer::ReceiverLoop, this);
        RecordLifecycleEvent("StartRequestChannelAndReceiver");
    } catch (const std::exception& e) {
        return UC::Status::Error(std::string{"failed to start Receiver: "} + e.what());
    }
    return UC::Status::OK();
}

void DramPoolServer::SetServiceReady(bool ready)
{
    serviceReady_.store(ready, std::memory_order_release);
    RecordLifecycleEvent(ready ? "SetServiceReady(true)" : "SetServiceReady(false)");
}

void DramPoolServer::StopReceiver()
{
    receiverStop_.store(true, std::memory_order_release);
    stopWaitCv_.notify_all();
    if (receiverThread_.joinable()) { receiverThread_.join(); }
    RecordLifecycleEvent("StopReceiver");
}

void DramPoolServer::StopTaskWorker()
{
    taskWorkerStop_.store(true, std::memory_order_release);
    if (taskWorkerThread_.joinable()) { taskWorkerThread_.join(); }
    taskWorker_.reset();
    RecordLifecycleEvent("StopTaskWorker");
}

void DramPoolServer::CancelInflightTransports()
{
    if (completionPoller_) { completionPoller_->RequestCancelAll(); }
    RecordLifecycleEvent("CancelInflightTransports");
}

void DramPoolServer::StopCompletionPoller()
{
    completionPollerStop_.store(true, std::memory_order_release);
    if (completionPollerThread_.joinable()) { completionPollerThread_.join(); }
    if (completionPoller_ && !completionPoller_->Healthy()) {
        UC_ERROR_UNLIMITED("DramPool CompletionPoller stopped in failed state");
    }
    completionPoller_.reset();
    RecordLifecycleEvent("StopCompletionPoller");
}

void DramPoolServer::StopGCThread()
{
    gcThreadStop_.store(true, std::memory_order_release);
    stopWaitCv_.notify_all();
    if (gcThread_.joinable()) { gcThread_.join(); }
    if (config_.gcEnabled) { RecordLifecycleEvent("StopGCThread"); }
}

void DramPoolServer::UnregisterBufferMemory()
{
    bufferManagers_.clear();
    RecordLifecycleEvent("UnregisterBufferMemory");
}

void DramPoolServer::DestroyMetadataIndex()
{
    metadataIndex_.reset();
    RecordLifecycleEvent("DestroyMetadataIndex");
}

void DramPoolServer::ReceiverLoop()
{
    UC_INFO_UNLIMITED("DramPool Receiver started, listen_addr={}", config_.listenAddr);
    std::unique_lock<std::mutex> waitLock(stopWaitMutex_);
    stopWaitCv_.wait(waitLock, [this]() { return receiverStop_.load(std::memory_order_acquire); });
    UC_INFO_UNLIMITED("DramPool Receiver stopped");
}

void DramPoolServer::TaskWorkerLoop()
{
    UC_INFO_UNLIMITED("DramPool TaskWorker started");
    taskWorker_->Run(taskWorkerStop_);
    UC_INFO_UNLIMITED("DramPool TaskWorker stopped");
}

void DramPoolServer::CompletionPollerLoop()
{
    UC_INFO_UNLIMITED("DramPool CompletionPoller started");
    completionPoller_->Run(completionPollerStop_);
    UC_INFO_UNLIMITED("DramPool CompletionPoller stopped");
}

void DramPoolServer::GCThreadLoop()
{
    UC_INFO_UNLIMITED("DramPool GCThread started, interval_ms={}", config_.gcIntervalMs);
    const auto interval = std::chrono::milliseconds(config_.gcIntervalMs);
    std::unique_lock<std::mutex> waitLock(stopWaitMutex_);
    while (!gcThreadStop_.load(std::memory_order_acquire)) {
        stopWaitCv_.wait_for(waitLock, interval,
                             [this]() { return gcThreadStop_.load(std::memory_order_acquire); });
    }
    UC_INFO_UNLIMITED("DramPool GCThread stopped");
}

void DramPoolServer::RecordLifecycleEvent(const std::string& event)
{
    std::lock_guard<std::mutex> guard(lifecycleMutex_);
    lifecycleEvents_.push_back(event);
}

void DramPoolServer::ResetInitializedComponents()
{
    completionPoller_.reset();
    taskWorker_.reset();
    protocolManager_.reset();
    metadataIndex_.reset();
    bufferManagers_.clear();
    transportManager_.reset();
    g_services = {};
}

}  // namespace UC::DRAMPOOL
