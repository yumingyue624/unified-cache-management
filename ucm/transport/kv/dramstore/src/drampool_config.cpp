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
#include "drampool_config.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace UC::DRAMPOOL {
namespace {

bool IsLongOption(const std::string& value)
{ return value.size() > 2 && value.rfind("--", 0) == 0; }

std::string Trim(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) { return ""; }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::uint64_t ParseUint64(const std::string& value)
{
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument("expected an unsigned integer");
    }
    std::size_t parsed = 0;
    const auto number = std::stoull(value, &parsed, 0);
    if (parsed != value.size()) { throw std::invalid_argument("trailing characters"); }
    return number;
}

std::uint32_t ParseUint32(const std::string& value)
{
    const auto number = ParseUint64(value);
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::out_of_range("uint32 overflow");
    }
    return static_cast<std::uint32_t>(number);
}

UC::Status ReadSingleValue(const std::string& option, bool hasInlineValue,
                           const std::string& inlineValue, int argc, char** argv, int& index,
                           std::string& value)
{
    if (hasInlineValue) {
        if (inlineValue.empty()) {
            return UC::Status::InvalidParam("{} requires a value", option);
        }
        value = inlineValue;
        return UC::Status::OK();
    }

    if (++index >= argc || argv[index] == nullptr || IsLongOption(argv[index])) {
        return UC::Status::InvalidParam("{} requires a value", option);
    }
    value = argv[index];
    if (value.empty()) { return UC::Status::InvalidParam("{} requires a value", option); }
    return UC::Status::OK();
}

UC::Status ReadListValues(const std::string& option, bool hasInlineValue,
                          const std::string& inlineValue, int argc, char** argv, int& index,
                          std::vector<std::string>& values)
{
    values.clear();
    if (hasInlineValue) {
        if (inlineValue.empty()) {
            return UC::Status::InvalidParam("{} requires at least one value", option);
        }
        values.emplace_back(inlineValue);
    }

    while (index + 1 < argc && argv[index + 1] != nullptr && !IsLongOption(argv[index + 1])) {
        const std::string value = argv[++index];
        if (value.empty()) { return UC::Status::InvalidParam("{} has an empty value", option); }
        values.emplace_back(value);
    }
    if (values.empty()) {
        return UC::Status::InvalidParam("{} requires at least one value", option);
    }
    return UC::Status::OK();
}

UC::Status ParseBlockSizes(const std::vector<std::string>& values,
                           std::vector<std::uint64_t>& sizes)
{
    try {
        sizes.clear();
        sizes.reserve(values.size());
        for (const auto& value : values) { sizes.emplace_back(ParseUint64(value)); }
    } catch (const std::exception& error) {
        return UC::Status::InvalidParam("invalid --kvcache-block-sizes value: {}", error.what());
    }
    return UC::Status::OK();
}

UC::Status ParseBlockProportions(const std::vector<std::string>& values,
                                 std::vector<std::uint32_t>& proportions)
{
    try {
        proportions.clear();
        proportions.reserve(values.size());
        for (const auto& value : values) { proportions.emplace_back(ParseUint32(value)); }
    } catch (const std::exception& error) {
        return UC::Status::InvalidParam(
            "invalid --kvcache-block-proportions value: {}", error.what());
    }
    return UC::Status::OK();
}

UC::Status RequirePositive(const char* name, std::uint64_t value)
{
    if (value == 0) { return UC::Status::InvalidParam("{} must be greater than zero", name); }
    return UC::Status::OK();
}

UC::Status RequireAtLeast(const char* name, std::uint64_t value, std::uint64_t minimum)
{
    if (value < minimum) {
        return UC::Status::InvalidParam("{} must be at least {}", name, minimum);
    }
    return UC::Status::OK();
}

}  // namespace

std::string BuildUsage(const char* program)
{
    const std::string name = program == nullptr ? "drampool" : program;
    return "Usage: " + name +
           " --addr <IP>:<PORT> --nics <NAME>... --pool-size-gb <SIZE>"
           " --kvcache-block-sizes <SIZE>... [options]\n"
           "Required options:\n"
           "  --addr <IP>:<PORT>                 DramPool control-service address.\n"
           "  --nics <NAME>...                   RDMA NIC names for pinned memory registration.\n"
           "  --pool-size-gb <SIZE>              Local DRAM capacity contributed to the pool.\n"
           "  --kvcache-block-sizes <SIZE>...    Supported fixed KVCache block sizes.\n"
           "Optional options:\n"
           "  --kvcache-block-proportions <P>... Relative capacity per block size; defaults to 1:1.\n"
           "  --ttl-minutes <MINUTES>            Absolute block lifetime; defaults to 120.\n";
}

UC::Status ParseCommandLine(int argc, char** argv, DramPoolConfig& config)
{
    config = DramPoolConfig{};

    bool hasAddr = false;
    bool hasNics = false;
    bool hasPoolSize = false;
    bool hasBlockSizes = false;
    bool hasBlockProportions = false;
    bool hasTtl = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] == nullptr ? "" : argv[index];
        if (!IsLongOption(argument)) {
            return UC::Status::InvalidParam("unexpected argument: {}", argument);
        }

        const auto equals = argument.find('=');
        const std::string option = argument.substr(0, equals);
        const bool hasInlineValue = equals != std::string::npos;
        const std::string inlineValue =
            hasInlineValue ? argument.substr(equals + 1) : std::string{};
        std::string value;
        std::vector<std::string> values;
        auto status = UC::Status::OK();

        if (option == "--addr") {
            if (hasAddr) { return UC::Status::InvalidParam("--addr may be specified once"); }
            status = ReadSingleValue(option, hasInlineValue, inlineValue, argc, argv, index, value);
            if (status.Failure()) { return status; }
            config.addr = value;
            hasAddr = true;
            continue;
        }
        if (option == "--nics") {
            if (hasNics) { return UC::Status::InvalidParam("--nics may be specified once"); }
            status = ReadListValues(option, hasInlineValue, inlineValue, argc, argv, index, values);
            if (status.Failure()) { return status; }
            config.nics = std::move(values);
            hasNics = true;
            continue;
        }
        if (option == "--pool-size-gb") {
            if (hasPoolSize) {
                return UC::Status::InvalidParam("--pool-size-gb may be specified once");
            }
            status = ReadSingleValue(option, hasInlineValue, inlineValue, argc, argv, index, value);
            if (status.Failure()) { return status; }
            try {
                config.poolSizeGb = ParseUint64(value);
            } catch (const std::exception& error) {
                return UC::Status::InvalidParam("invalid --pool-size-gb value: {}", error.what());
            }
            hasPoolSize = true;
            continue;
        }
        if (option == "--kvcache-block-sizes") {
            if (hasBlockSizes) {
                return UC::Status::InvalidParam("--kvcache-block-sizes may be specified once");
            }
            status = ReadListValues(option, hasInlineValue, inlineValue, argc, argv, index, values);
            if (status.Failure()) { return status; }
            status = ParseBlockSizes(values, config.poolBlockSizes);
            if (status.Failure()) { return status; }
            hasBlockSizes = true;
            continue;
        }
        if (option == "--kvcache-block-proportions") {
            if (hasBlockProportions) {
                return UC::Status::InvalidParam(
                    "--kvcache-block-proportions may be specified once");
            }
            status = ReadListValues(option, hasInlineValue, inlineValue, argc, argv, index, values);
            if (status.Failure()) { return status; }
            status = ParseBlockProportions(values, config.poolBlockProportions);
            if (status.Failure()) { return status; }
            hasBlockProportions = true;
            continue;
        }
        if (option == "--ttl-minutes") {
            if (hasTtl) {
                return UC::Status::InvalidParam("--ttl-minutes may be specified once");
            }
            status = ReadSingleValue(option, hasInlineValue, inlineValue, argc, argv, index, value);
            if (status.Failure()) { return status; }
            try {
                const auto ttlMinutes = ParseUint64(value);
                if (ttlMinutes == 0 ||
                    ttlMinutes > std::numeric_limits<std::uint64_t>::max() /
                                     kMillisecondsPerMinute) {
                    return UC::Status::InvalidParam("--ttl-minutes must be a positive value");
                }
                config.defaultDumpTtlMs = ttlMinutes * kMillisecondsPerMinute;
            } catch (const std::exception& error) {
                return UC::Status::InvalidParam("invalid --ttl-minutes value: {}", error.what());
            }
            hasTtl = true;
            continue;
        }
        return UC::Status::InvalidParam("unknown argument: {}", argument);
    }

    if (!hasAddr || !hasNics || !hasPoolSize || !hasBlockSizes) {
        return UC::Status::InvalidParam(
            "--addr, --nics, --pool-size-gb, and --kvcache-block-sizes are required");
    }
    if (!hasBlockProportions) {
        // Each configured block size receives an equal capacity share by default.
        config.poolBlockProportions.assign(config.poolBlockSizes.size(), 1);
    }
    return ValidateDramPoolConfig(config);
}

UC::Status ValidateDramPoolConfig(const DramPoolConfig& config)
{
    if (Trim(config.serverId).empty()) { return UC::Status::InvalidParam("server.id is required"); }
    if (Trim(config.addr).empty()) { return UC::Status::InvalidParam("--addr is required"); }
    if (config.nics.empty()) { return UC::Status::InvalidParam("--nics must not be empty"); }
    for (const auto& nic : config.nics) {
        if (Trim(nic).empty()) { return UC::Status::InvalidParam("--nics contains an empty name"); }
    }

    const auto transportMode = ToLower(config.transportMode);
    if (transportMode != "hixl") {
        return UC::Status::InvalidParam("unsupported transport.mode: {}", config.transportMode);
    }
    if (Trim(config.transportLocalEngine).empty()) {
        return UC::Status::InvalidParam("transport.local_engine is required");
    }
    if (config.transportDeviceId < 0) {
        return UC::Status::InvalidParam("transport.device_id must not be negative");
    }

    auto status = RequirePositive("--pool-size-gb", config.poolSizeGb);
    if (status.Failure()) { return status; }
    if (config.poolBlockSizes.empty()) {
        return UC::Status::InvalidParam("--kvcache-block-sizes must not be empty");
    }
    if (config.poolBlockProportions.empty()) {
        return UC::Status::InvalidParam("--kvcache-block-proportions must not be empty");
    }
    if (config.poolBlockSizes.size() != config.poolBlockProportions.size()) {
        return UC::Status::InvalidParam(
            "--kvcache-block-sizes and --kvcache-block-proportions must have the same length");
    }
    for (const auto blockSize : config.poolBlockSizes) {
        status = RequirePositive("--kvcache-block-sizes item", blockSize);
        if (status.Failure()) { return status; }
    }
    for (const auto proportion : config.poolBlockProportions) {
        status = RequirePositive("--kvcache-block-proportions item", proportion);
        if (status.Failure()) { return status; }
    }
    status = RequirePositive("--ttl-minutes", config.defaultDumpTtlMs);
    if (status.Failure()) { return status; }

    status = RequirePositive("metadata.shards", config.metadataShards);
    if (status.Failure()) { return status; }
    status = RequireAtLeast("queue.request_depth", config.requestQueueDepth, 2);
    if (status.Failure()) { return status; }
    status = RequireAtLeast("queue.handle_depth", config.handleQueueDepth, 2);
    if (status.Failure()) { return status; }
    status = RequirePositive("poller.drain_budget", config.pollerDrainBudget);
    if (status.Failure()) { return status; }
    status = RequirePositive("poller.scan_budget", config.pollerScanBudget);
    if (status.Failure()) { return status; }
    status = RequirePositive("poller.max_pending", config.pollerMaxPending);
    if (status.Failure()) { return status; }
    status = RequirePositive("poller.idle_wait_us", config.pollerIdleWaitUs);
    if (status.Failure()) { return status; }
    if (config.pollerMaxPending < config.pollerDrainBudget ||
        config.pollerMaxPending < config.pollerScanBudget) {
        return UC::Status::InvalidParam(
            "poller.max_pending must be greater than or equal to poller budgets");
    }
    status = RequirePositive("op.timeout_ms", config.opTimeoutMs);
    if (status.Failure()) { return status; }
    if (config.opTimeoutMs > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return UC::Status::InvalidParam("op.timeout_ms exceeds transport int32 range");
    }
    status = RequirePositive("shutdown.timeout_ms", config.shutdownTimeoutMs);
    if (status.Failure()) { return status; }
    if (config.gcEnabled) {
        status = RequirePositive("gc.interval_ms", config.gcIntervalMs);
        if (status.Failure()) { return status; }
    }

    return UC::Status::OK();
}

}  // namespace UC::DRAMPOOL
