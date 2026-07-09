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

namespace UC::DRAMPOOL {
namespace {

constexpr auto kIdleSleep = std::chrono::milliseconds(10);

}  // namespace

DramPoolServer::~DramPoolServer() { Stop(); }

UC::Status DramPoolServer::Init(const DramPoolConfig& config)
{
    if (initialized_.load(std::memory_order_acquire)) {
        return UC::Status::InvalidParam("DramPoolServer is already initialized");
    }
    auto status = ValidateDramPoolConfig(config);
    if (status.Failure()) { return status; }

    config_ = config;

    status = InitDataTransportManager();
    if (status.Failure()) { return status; }
    status = InstallDataTransport();
    if (status.Failure()) { return status; }
    status = InitBufferMgr();
    if (status.Failure()) { return status; }
    status = RegisterBufferMemory();
    if (status.Failure()) { return status; }
    status = InitMetadataIndex();
    if (status.Failure()) { return status; }
    status = InitProtocol();
    if (status.Failure()) { return status; }
    status = InitQueues();
    if (status.Failure()) { return status; }

    initialized_.store(true, std::memory_order_release);
    return UC::Status::OK();
}

UC::Status DramPoolServer::Start()
{
    if (!initialized_.load(std::memory_order_acquire)) {
        return UC::Status::InvalidParam("DramPoolServer is not initialized");
    }
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return UC::Status::InvalidParam("DramPoolServer is already started");
    }

    auto status = StartCompletionPoller();
    if (status.Failure()) {
        Stop();
        return status;
    }
    status = StartTaskWorker();
    if (status.Failure()) {
        Stop();
        return status;
    }
    status = StartGCThread();
    if (status.Failure()) {
        Stop();
        return status;
    }
    status = StartRequestChannelAndReceiver();
    if (status.Failure()) {
        Stop();
        return status;
    }
    SetServiceReady(true);
    return UC::Status::OK();
}

void DramPoolServer::Stop()
{
    if (!started_.exchange(false, std::memory_order_acq_rel) &&
        !initialized_.load(std::memory_order_acquire)) {
        return;
    }

    SetServiceReady(false);
    StopReceiver();
    StopTaskWorker();
    CancelInflightTransports();
    StopCompletionPoller();
    StopGCThread();
    UnregisterBufferMemory();
    DestroyMetadataIndex();
    initialized_.store(false, std::memory_order_release);
}

bool DramPoolServer::IsServiceReady() const noexcept
{
    return serviceReady_.load(std::memory_order_acquire);
}

std::vector<std::string> DramPoolServer::LifecycleEvents() const
{
    std::lock_guard<std::mutex> guard(lifecycleMutex_);
    return lifecycleEvents_;
}

UC::Status DramPoolServer::InitDataTransportManager()
{
    RecordLifecycleEvent("InitDataTransportManager");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InstallDataTransport()
{
    RecordLifecycleEvent("InstallDataTransport");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InitBufferMgr()
{
    RecordLifecycleEvent("InitBufferMgr");
    return UC::Status::OK();
}

UC::Status DramPoolServer::RegisterBufferMemory()
{
    RecordLifecycleEvent("RegisterBufferMemory");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InitMetadataIndex()
{
    RecordLifecycleEvent("InitMetadataIndex");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InitProtocol()
{
    RecordLifecycleEvent("InitProtocol");
    return UC::Status::OK();
}

UC::Status DramPoolServer::InitQueues()
{
    RecordLifecycleEvent("InitQueues");
    return UC::Status::OK();
}

UC::Status DramPoolServer::StartCompletionPoller()
{
    try {
        completionPollerStop_.store(false, std::memory_order_release);
        completionPollerThread_ = std::thread(&DramPoolServer::CompletionPollerLoop, this);
        RecordLifecycleEvent("StartCompletionPoller");
    } catch (const std::exception& e) {
        return UC::Status::Error(std::string{"failed to start CompletionPoller: "} + e.what());
    }
    return UC::Status::OK();
}

UC::Status DramPoolServer::StartTaskWorker()
{
    try {
        taskWorkerStop_.store(false, std::memory_order_release);
        taskWorkerThread_ = std::thread(&DramPoolServer::TaskWorkerLoop, this);
        RecordLifecycleEvent("StartTaskWorker");
    } catch (const std::exception& e) {
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
    if (receiverThread_.joinable()) { receiverThread_.join(); }
    RecordLifecycleEvent("StopReceiver");
}

void DramPoolServer::StopTaskWorker()
{
    taskWorkerStop_.store(true, std::memory_order_release);
    if (taskWorkerThread_.joinable()) { taskWorkerThread_.join(); }
    RecordLifecycleEvent("StopTaskWorker");
}

void DramPoolServer::CancelInflightTransports() { RecordLifecycleEvent("CancelInflightTransports"); }

void DramPoolServer::StopCompletionPoller()
{
    completionPollerStop_.store(true, std::memory_order_release);
    if (completionPollerThread_.joinable()) { completionPollerThread_.join(); }
    RecordLifecycleEvent("StopCompletionPoller");
}

void DramPoolServer::StopGCThread()
{
    gcThreadStop_.store(true, std::memory_order_release);
    if (gcThread_.joinable()) { gcThread_.join(); }
    if (config_.gcEnabled) { RecordLifecycleEvent("StopGCThread"); }
}

void DramPoolServer::UnregisterBufferMemory() { RecordLifecycleEvent("UnregisterBufferMemory"); }

void DramPoolServer::DestroyMetadataIndex() { RecordLifecycleEvent("DestroyMetadataIndex"); }

void DramPoolServer::ReceiverLoop()
{
    UC_INFO_UNLIMITED("DramPool Receiver started, listen_addr={}", config_.listenAddr);
    while (!receiverStop_.load(std::memory_order_acquire)) { std::this_thread::sleep_for(kIdleSleep); }
    UC_INFO_UNLIMITED("DramPool Receiver stopped");
}

void DramPoolServer::TaskWorkerLoop()
{
    UC_INFO_UNLIMITED("DramPool TaskWorker started");
    while (!taskWorkerStop_.load(std::memory_order_acquire)) { std::this_thread::sleep_for(kIdleSleep); }
    UC_INFO_UNLIMITED("DramPool TaskWorker stopped");
}

void DramPoolServer::CompletionPollerLoop()
{
    UC_INFO_UNLIMITED("DramPool CompletionPoller started");
    while (!completionPollerStop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kIdleSleep);
    }
    UC_INFO_UNLIMITED("DramPool CompletionPoller stopped");
}

void DramPoolServer::GCThreadLoop()
{
    UC_INFO_UNLIMITED("DramPool GCThread started, interval_ms={}", config_.gcIntervalMs);
    const auto interval = std::chrono::milliseconds(config_.gcIntervalMs);
    while (!gcThreadStop_.load(std::memory_order_acquire)) { std::this_thread::sleep_for(interval); }
    UC_INFO_UNLIMITED("DramPool GCThread stopped");
}

void DramPoolServer::RecordLifecycleEvent(const std::string& event)
{
    std::lock_guard<std::mutex> guard(lifecycleMutex_);
    lifecycleEvents_.push_back(event);
}

}  // namespace UC::DRAMPOOL
