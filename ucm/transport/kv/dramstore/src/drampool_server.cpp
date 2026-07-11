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
#include <acl/acl.h>
#include <chrono>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>
#include "logger.h"
#include "task_worker.h"

namespace UC::DRAMPOOL {
namespace {

template <typename Callback>
class ScopeExit final {
public:
    explicit ScopeExit(Callback callback) : callback_(std::move(callback)) {}
    ~ScopeExit()
    {
        if (active_) { callback_(); }
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    ScopeExit(ScopeExit&& other) noexcept(std::is_nothrow_move_constructible_v<Callback>)
        : callback_(std::move(other.callback_)), active_(std::exchange(other.active_, false))
    {
    }

    void Release() noexcept { active_ = false; }

private:
    Callback callback_;
    bool active_{true};
};

template <typename Callback>
auto MakeScopeExit(Callback&& callback)
{
    return ScopeExit<std::decay_t<Callback>>(std::forward<Callback>(callback));
}

UC::Status InitializeAclRuntime(std::int32_t deviceId, bool& ownsRuntime)
{
    const auto initStatus = aclInit(nullptr);
    if (initStatus == ACL_SUCCESS) {
        ownsRuntime = true;
    } else if (initStatus != ACL_ERROR_REPEAT_INITIALIZE) {
        return UC::Status::Error("aclInit failed: " + std::to_string(initStatus));
    }

    const auto setDeviceStatus = aclrtSetDevice(deviceId);
    if (setDeviceStatus == ACL_SUCCESS) { return UC::Status::OK(); }

    if (ownsRuntime) {
        (void)aclFinalize();
        ownsRuntime = false;
    }
    return UC::Status::Error("aclrtSetDevice failed: " + std::to_string(setDeviceStatus));
}

}  // namespace

DramPoolServer::DramPoolServer() = default;

DramPoolServer::~DramPoolServer() { Stop(); }

UC::Status DramPoolServer::Init()
{
    std::lock_guard<std::mutex> controlGuard(controlMutex_);
    if (state_ != ServerState::New) {
        return UC::Status::InvalidParam(
            "DramPoolServer cannot be initialized in its current state");
    }

    // Build local state only; externally visible services start in Start().
    auto rollback = MakeScopeExit([this]() { ResetInitializedComponents(); });
    try {
        if (auto status = InitMemoryPool(); status.Failure()) { return status; }
        if (auto status = InitMetadata(); status.Failure()) { return status; }
        if (auto status = InitProtocol(); status.Failure()) { return status; }
        if (auto status = InitQueues(); status.Failure()) { return status; }
    } catch (const std::exception& error) {
        return UC::Status::Error(std::string{"DramPoolServer initialization failed: "} +
                                 error.what());
    }

    state_ = ServerState::Initialized;
    rollback.Release();
    return UC::Status::OK();
}

UC::Status DramPoolServer::Start()
{
    std::lock_guard<std::mutex> controlGuard(controlMutex_);
    if (state_ != ServerState::Initialized) {
        return UC::Status::InvalidParam("DramPoolServer is not ready to start");
    }
    state_ = ServerState::Starting;

    auto rollback = MakeScopeExit([this]() { StopLocked(); });
    try {
        if (auto status = StartTransportService(); status.Failure()) { return status; }
        if (auto status = CreateRuntimeContext(); status.Failure()) { return status; }
        if (auto status = StartCompletionPoller(); status.Failure()) { return status; }
        if (auto status = StartTaskWorker(); status.Failure()) { return status; }
        if (auto status = StartGCThread(); status.Failure()) { return status; }
        if (auto status = StartListeningService(); status.Failure()) { return status; }
        SetServiceReady(true);
        state_ = ServerState::Ready;
    } catch (const std::exception& error) {
        return UC::Status::Error(std::string{"DramPoolServer start failed: "} + error.what());
    }
    rollback.Release();
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
    MarkInflightTransportsFailed();
    StopCompletionPoller();
    StopGCThread();
    runtime_.reset();
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

UC::Status DramPoolServer::InitMemoryPool()
{
    if (const auto status = InitializeAclRuntime(g_config.transportDeviceId, aclRuntimeOwned_);
        status.Failure()) {
        return status;
    }
    constexpr char kBufferPoolNamePrefix[] = "drampool-kvcache-";
    for (std::size_t index = 0; index < g_config.poolBlockSizes.size(); ++index) {
        auto bufferPool = std::make_unique<AsuBufferPool>();
        if (const auto status = bufferPool->Init(
                kBufferPoolNamePrefix + std::to_string(index),
                static_cast<std::size_t>(g_config.poolBlockSizes[index]),
                static_cast<std::size_t>(g_config.poolSlotCounts[index]));
            status.Failure()) {
            return status;
        }
        bufferPools_.push_back(std::move(bufferPool));
    }
    RecordLifecycleEvent("InitMemoryPool");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InitMetadata()
{
    metadataIndex_ = std::make_unique<FakeMetadataIndex>();
    RecordLifecycleEvent("InitMetadata");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InitProtocol()
{
    protocolManager_ = std::make_unique<ProtocolManager>();
    RecordLifecycleEvent("InitProtocol");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InitQueues()
{
    requestQueue_.Setup(g_config.requestQueueDepth);
    transHandleQueue_.Setup(g_config.handleQueueDepth);
    RecordLifecycleEvent("InitQueues");
    return UC::Status::OK();
}

UC::Status DramPoolServer::StartTransportService()
{
    transportManager_ = std::make_unique<transport::TransportManager>(g_config.addr);
    auto status = transportManager_->Init();
    if (status != transport::Status::Ok) {
        transportManager_.reset();
        return ToUcStatus(status, "TransportManager::Init");
    }

    transport::HixlInitAttrs attrs;
    attrs.local_engine = g_config.transportLocalEngine;
    attrs.device_id = g_config.transportDeviceId;
    attrs.connect_timeout_ms = static_cast<std::int32_t>(g_config.opTimeoutMs);
    attrs.transfer_timeout_ms = static_cast<std::int32_t>(g_config.opTimeoutMs);
    status = transportManager_->InstallTransport(transport::TransportProtocol::Hixl, attrs);
    if (status != transport::Status::Ok) {
        return ToUcStatus(status, "TransportManager::InstallTransport");
    }
    if (bufferPools_.empty()) {
        return UC::Status::InvalidParam("memory pool is not initialized");
    }
    RecordLifecycleEvent("StartTransportService");
    return UC::Status::OK();
}

UC::Status DramPoolServer::CreateRuntimeContext()
{
    if (!metadataIndex_ || !protocolManager_ || !transportManager_) {
        return UC::Status::InvalidParam("DramPool runtime dependencies are not initialized");
    }
    if (bufferPools_.empty()) {
        return UC::Status::InvalidParam("DramPool buffer pools are not initialized");
    }
    try {
        runtime_ = std::make_unique<DramPoolRuntime>(*metadataIndex_, bufferPools_,
                                                      *transportManager_, *protocolManager_,
                                                      requestQueue_, transHandleQueue_);
    } catch (const std::exception& error) {
        return UC::Status::Error(std::string{"failed to create DramPool runtime: "} +
                                 error.what());
    }
    RecordLifecycleEvent("CreateRuntimeContext");
    return UC::Status::OK();
}

UC::Status DramPoolServer::StartCompletionPoller()
{
    if (!runtime_) { return UC::Status::InvalidParam("DramPool runtime is not initialized"); }
    try {
        completionPoller_ = std::make_unique<CompletionPoller>(*runtime_);
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
    if (!runtime_) { return UC::Status::InvalidParam("DramPool runtime is not initialized"); }
    try {
        taskWorker_ = std::make_unique<TaskWorker>(*runtime_);
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
    if (!g_config.gcEnabled) { return UC::Status::OK(); }
    try {
        gcThreadStop_.store(false, std::memory_order_release);
        gcThread_ = std::thread(&DramPoolServer::GCThreadLoop, this);
        RecordLifecycleEvent("StartGCThread");
    } catch (const std::exception& e) {
        return UC::Status::Error(std::string{"failed to start GCThread: "} + e.what());
    }
    return UC::Status::OK();
}

UC::Status DramPoolServer::StartListeningService()
{
    try {
        receiverStop_.store(false, std::memory_order_release);
        receiverThread_ = std::thread(&DramPoolServer::ReceiverLoop, this);
        RecordLifecycleEvent("StartListeningService");
    } catch (const std::exception& e) {
        return UC::Status::Error(std::string{"failed to start listening service: "} + e.what());
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

void DramPoolServer::MarkInflightTransportsFailed()
{
    if (completionPoller_) { completionPoller_->RequestDrainAllAsFailed(); }
    RecordLifecycleEvent("MarkInflightTransportsFailed");
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
    if (g_config.gcEnabled) { RecordLifecycleEvent("StopGCThread"); }
}

void DramPoolServer::UnregisterBufferMemory()
{
    if (transportManager_) {
        const auto status = transportManager_->Shutdown();
        if (status != transport::Status::Ok) {
            UC_ERROR_UNLIMITED("DramPool TransportManager shutdown failed");
        }
    }
    bufferPools_.clear();
    RecordLifecycleEvent("UnregisterBufferMemory");
}

void DramPoolServer::DestroyMetadataIndex()
{
    metadataIndex_.reset();
    RecordLifecycleEvent("DestroyMetadataIndex");
}

void DramPoolServer::ReceiverLoop()
{
    UC_INFO_UNLIMITED("DramPool Receiver started, addr={}", g_config.addr);
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
    UC_INFO_UNLIMITED("DramPool GCThread started, interval_ms={}",
                      g_config.gcIntervalMs);
    const auto interval = std::chrono::milliseconds(g_config.gcIntervalMs);
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
    runtime_.reset();
    completionPoller_.reset();
    taskWorker_.reset();
    protocolManager_.reset();
    metadataIndex_.reset();
    transportManager_.reset();
    bufferPools_.clear();
    if (aclRuntimeOwned_) {
        (void)aclrtResetDevice(g_config.transportDeviceId);
        (void)aclFinalize();
        aclRuntimeOwned_ = false;
    }
}

}  // namespace UC::DRAMPOOL
