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
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include "logger.h"

namespace UC::DRAMPOOL {
namespace {

constexpr char kUcmLogPathEnv[] = "UCM_LOG_PATH";
constexpr char kUcmLogMaxFilesEnv[] = "UCM_LOG_MAX_FILES";
constexpr char kUcmLogMaxSizeEnv[] = "UCM_LOG_MAX_SIZE";
constexpr char kDefaultLogPath[] = "log";
constexpr int kDefaultLogFileCount = 10;
constexpr int kDefaultLogFileSizeMb = 5;
constexpr auto kShutdownPollInterval = std::chrono::milliseconds(100);

std::string ReadEnvironment(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : value;
}

std::string ReadEnvironmentString(const char* name, const char* defaultValue)
{
    const auto value = ReadEnvironment(name);
    return value.empty() ? defaultValue : value;
}

int ReadPositiveEnvironmentInt(const char* name, int defaultValue)
{
    const auto value = ReadEnvironment(name);
    if (value.empty()) { return defaultValue; }

    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || parsed <= 0 ||
        parsed > std::numeric_limits<int>::max()) {
        return defaultValue;
    }
    return static_cast<int>(parsed);
}

}  // namespace

std::atomic_bool DramPoolDaemon::shutdownRequested_{false};

int DramPoolDaemon::Run(int argc, char** argv)
{
    auto status = ParseCommandLine(argc, argv, g_config);
    if (status.Failure()) {
        std::cerr << status.ToString() << "\n" << BuildUsage(argc > 0 ? argv[0] : "drampool");
        return 1;
    }
    status = SetupLogger();
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
    status = server.Init();
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

    UC_INFO_UNLIMITED("DramPool service ready, addr={}", g_config.addr);
    WaitForShutdown();
    UC_INFO_UNLIMITED("DramPool shutdown requested");
    server.Stop();
    UC::Logger::Flush();
    return 0;
}

UC::Status DramPoolDaemon::SetupLogger()
{
    // Match UCM's process-wide environment contract; UC_LOGGER_LEVEL is read by UC::Logger.
    const auto logPath = ReadEnvironmentString(kUcmLogPathEnv, kDefaultLogPath);
    const auto maxFiles = ReadPositiveEnvironmentInt(kUcmLogMaxFilesEnv, kDefaultLogFileCount);
    const auto maxSize = ReadPositiveEnvironmentInt(kUcmLogMaxSizeEnv, kDefaultLogFileSizeMb);
    UC::Logger::Setup(logPath, maxFiles, maxSize);
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
