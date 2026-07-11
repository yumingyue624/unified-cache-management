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

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "status/status.h"

namespace UC::DRAMPOOL {

inline constexpr std::uint32_t kDefaultPollerDrainBudget = 64;
inline constexpr std::uint32_t kDefaultPollerScanBudget = 64;
inline constexpr std::uint32_t kDefaultPollerMaxPending = 1024 * 1024;
inline constexpr std::uint32_t kDefaultPollerIdleWaitUs = 100;
inline constexpr std::uint64_t kMillisecondsPerMinute = 60'000;
inline constexpr std::uint64_t kDefaultTtlMinutes = 120;
inline constexpr std::uint64_t kDefaultDumpTtlMs =
    kDefaultTtlMinutes * kMillisecondsPerMinute;

struct DramPoolConfig {
    std::string listenAddr;
    std::vector<std::string> nics{};
    std::uint64_t poolSizeGb{0};
    std::vector<std::uint64_t> poolBlockSizes{};
    std::vector<std::uint32_t> poolBlockProportions{};
    std::uint64_t defaultDumpTtlMs{kDefaultDumpTtlMs};

    std::string serverId{"drampool-0"};
    std::string transportMode{"hixl"};
    std::string transportManagerAddr;
    std::string transportLocalEngine{"drampool-0"};
    std::int32_t transportDeviceId{0};

    std::uint32_t metadataShards{1024};

    std::uint32_t requestQueueDepth{65536};
    std::uint32_t handleQueueDepth{65536};

    std::uint32_t pollerDrainBudget{kDefaultPollerDrainBudget};
    std::uint32_t pollerScanBudget{kDefaultPollerScanBudget};
    std::uint32_t pollerMaxPending{kDefaultPollerMaxPending};
    std::uint32_t pollerIdleWaitUs{kDefaultPollerIdleWaitUs};

    bool gcEnabled{true};
    std::uint32_t gcIntervalMs{1000};

    std::uint32_t opTimeoutMs{5000};
    std::uint32_t shutdownTimeoutMs{30000};

    std::unordered_map<std::string, std::string> extra;
};

inline DramPoolConfig g_drampool_config{};

std::string BuildUsage(const char* program);
UC::Status ParseCommandLine(int argc, char** argv, DramPoolConfig& config);
UC::Status ValidateDramPoolConfig(const DramPoolConfig& config);

}  // namespace UC::DRAMPOOL
