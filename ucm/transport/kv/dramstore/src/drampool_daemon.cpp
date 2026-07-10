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
#include "drampool_daemon.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
#include "logger.h"

namespace UC::DRAMPOOL {
namespace {

constexpr int kDefaultLogFileCount = 3;
constexpr int kDefaultLogFileSizeMb = 64;
constexpr auto kShutdownPollInterval = std::chrono::milliseconds(100);

}  // namespace

std::atomic_bool DramPoolDaemon::shutdownRequested_{false};

int DramPoolDaemon::Run(int argc, char** argv)
{
    CommandLineOptions options;
    auto status = ParseCommandLine(argc, argv, options);
    if (status.Failure()) {
        std::cerr << status.ToString() << "\n" << BuildUsage(argc > 0 ? argv[0] : "drampool");
        return 1;
    }
    if (options.showHelp) {
        std::cout << BuildUsage(argc > 0 ? argv[0] : "drampool");
        return 0;
    }

    auto loadedConfig = LoadDramPoolConfig(options.configPath);
    if (!loadedConfig) {
        std::cerr << loadedConfig.Error().ToString() << "\n";
        return 1;
    }

    const auto config = std::move(loadedConfig).Value();
    status = SetupLogger(config);
    if (status.Failure()) {
        std::cerr << status.ToString() << "\n";
        return 1;
    }
    status = SetupSignals();
    if (status.Failure()) {
        UC_ERROR_UNLIMITED("DramPool setup signals failed: {}", status);
        return 1;
    }

    DramPoolServer server;
    status = server.Init(config);
    if (status.Failure()) {
        UC_ERROR_UNLIMITED("DramPool server init failed: {}", status);
        return 1;
    }
    status = server.Start();
    if (status.Failure()) {
        UC_ERROR_UNLIMITED("DramPool server start failed: {}", status);
        server.Stop();
        return 1;
    }

    UC_INFO_UNLIMITED("DramPool service ready, server_id={}, listen_addr={}", config.serverId,
                      config.listenAddr);
    WaitForShutdown();
    UC_INFO_UNLIMITED("DramPool shutdown requested");
    server.Stop();
    UC::Logger::Flush();
    return 0;
}

UC::Status DramPoolDaemon::SetupLogger(const DramPoolConfig& config)
{
    UC::Logger::Setup(config.logDir, kDefaultLogFileCount, kDefaultLogFileSizeMb);
    return UC::Status::OK();
}

UC::Status DramPoolDaemon::SetupSignals()
{
    shutdownRequested_.store(false, std::memory_order_release);
    if (std::signal(SIGINT, &DramPoolDaemon::HandleSignal) == SIG_ERR) {
        return UC::Status::OsApiError("failed to install SIGINT handler");
    }
    if (std::signal(SIGTERM, &DramPoolDaemon::HandleSignal) == SIG_ERR) {
        return UC::Status::OsApiError("failed to install SIGTERM handler");
    }
    return UC::Status::OK();
}

void DramPoolDaemon::WaitForShutdown()
{
    while (!shutdownRequested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kShutdownPollInterval);
    }
}

void DramPoolDaemon::HandleSignal(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        shutdownRequested_.store(true, std::memory_order_release);
    }
}

}  // namespace UC::DRAMPOOL
