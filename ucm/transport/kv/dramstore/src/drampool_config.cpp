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
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace UC::DRAMPOOL {
namespace {

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

std::vector<std::string> Split(const std::string& value, char delimiter)
{
    std::vector<std::string> parts;
    std::stringstream stream{value};
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        part = Trim(part);
        if (!part.empty()) { parts.emplace_back(std::move(part)); }
    }
    return parts;
}

std::uint64_t ParseUint64(const std::string& value)
{
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

bool ParseBool(std::string value)
{
    value = ToLower(Trim(value));
    if (value == "1" || value == "true" || value == "yes" || value == "on") { return true; }
    if (value == "0" || value == "false" || value == "no" || value == "off") { return false; }
    throw std::invalid_argument("invalid bool");
}

std::vector<std::uint64_t> ParseUint64List(const std::string& value)
{
    std::vector<std::uint64_t> result;
    for (const auto& part : Split(value, ',')) { result.emplace_back(ParseUint64(part)); }
    return result;
}

std::vector<std::uint32_t> ParseUint32List(const std::string& value)
{
    std::vector<std::uint32_t> result;
    for (const auto& part : Split(value, ',')) { result.emplace_back(ParseUint32(part)); }
    return result;
}

UC::Status ApplyConfigValue(DramPoolConfig& config, const std::string& key, const std::string& value)
{
    try {
        if (key == "server.id") {
            config.serverId = value;
        } else if (key == "listen.addr") {
            config.listenAddr = value;
        } else if (key == "transport.mode") {
            config.transportMode = ToLower(value);
        } else if (key == "pool.size_gb") {
            config.poolSizeGb = ParseUint64(value);
        } else if (key == "pool.block_sizes") {
            config.poolBlockSizes = ParseUint64List(value);
        } else if (key == "pool.block_proportions") {
            config.poolBlockProportions = ParseUint32List(value);
        } else if (key == "metadata.shards") {
            config.metadataShards = ParseUint32(value);
        } else if (key == "queue.request_depth") {
            config.requestQueueDepth = ParseUint32(value);
        } else if (key == "queue.handle_depth") {
            config.handleQueueDepth = ParseUint32(value);
        } else if (key == "poller.drain_budget") {
            config.pollerDrainBudget = ParseUint32(value);
        } else if (key == "poller.scan_budget") {
            config.pollerScanBudget = ParseUint32(value);
        } else if (key == "poller.max_pending") {
            config.pollerMaxPending = ParseUint32(value);
        } else if (key == "gc.enabled") {
            config.gcEnabled = ParseBool(value);
        } else if (key == "gc.interval_ms") {
            config.gcIntervalMs = ParseUint32(value);
        } else if (key == "op.timeout_ms") {
            config.opTimeoutMs = ParseUint32(value);
        } else if (key == "shutdown.timeout_ms") {
            config.shutdownTimeoutMs = ParseUint32(value);
        } else if (key == "log.level") {
            config.logLevel = ToLower(value);
        } else if (key == "log.dir") {
            config.logDir = value;
        } else {
            config.extra[key] = value;
        }
    } catch (const std::exception& e) {
        return UC::Status::InvalidParam("invalid config value, key={}, value={}, error={}", key, value,
                                    e.what());
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
    if (value < minimum) { return UC::Status::InvalidParam("{} must be at least {}", name, minimum); }
    return UC::Status::OK();
}

}  // namespace

std::string BuildUsage(const char* program)
{
    std::string name = program == nullptr ? "drampool" : program;
    return "Usage: " + name + " --config <path>\n"
           "Options:\n"
           "  -c, --config <path>  Path to DramPool key=value config file.\n"
           "  -h, --help           Show this help message.\n";
}

UC::Status ParseCommandLine(int argc, char** argv, CommandLineOptions& options)
{
    options = CommandLineOptions{};
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index] == nullptr ? "" : argv[index];
        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
            return UC::Status::OK();
        }
        if (arg == "-c" || arg == "--config") {
            if (index + 1 >= argc) { return UC::Status::InvalidParam("{} requires a path", arg); }
            options.configPath = argv[++index];
            continue;
        }
        constexpr const char* kConfigPrefix = "--config=";
        if (arg.rfind(kConfigPrefix, 0) == 0) {
            options.configPath = arg.substr(std::string{kConfigPrefix}.size());
            continue;
        }
        return UC::Status::InvalidParam("unknown argument: {}", arg);
    }
    if (options.configPath.empty()) { return UC::Status::InvalidParam("missing --config <path>"); }
    return UC::Status::OK();
}

UC::Expected<DramPoolConfig> LoadDramPoolConfig(const std::string& configPath)
{
    std::ifstream configFile{configPath};
    if (!configFile.is_open()) {
        return UC::Status::Error("failed to open DramPool config, path=" + configPath);
    }

    DramPoolConfig config;
    std::string line;
    std::uint32_t lineNo = 0;
    while (std::getline(configFile, line)) {
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line[0] == '#') { continue; }

        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            return UC::Status::InvalidParam("invalid config line {}, missing '='", lineNo);
        }

        const auto key = Trim(line.substr(0, pos));
        const auto value = Trim(line.substr(pos + 1));
        if (key.empty()) { return UC::Status::InvalidParam("invalid config line {}, empty key", lineNo); }

        auto status = ApplyConfigValue(config, key, value);
        if (status.Failure()) { return status; }
    }

    auto status = ValidateDramPoolConfig(config);
    if (status.Failure()) { return status; }
    return std::move(config);
}

UC::Status ValidateDramPoolConfig(const DramPoolConfig& config)
{
    if (Trim(config.serverId).empty()) { return UC::Status::InvalidParam("server.id is required"); }
    if (Trim(config.listenAddr).empty()) { return UC::Status::InvalidParam("listen.addr is required"); }

    const auto transportMode = ToLower(config.transportMode);
    if (transportMode != "tcp" && transportMode != "rdma" && transportMode != "hixl") {
        return UC::Status::InvalidParam("unsupported transport.mode: {}", config.transportMode);
    }

    auto status = RequirePositive("pool.size_gb", config.poolSizeGb);
    if (status.Failure()) { return status; }
    if (config.poolBlockSizes.empty()) {
        return UC::Status::InvalidParam("pool.block_sizes must not be empty");
    }
    if (config.poolBlockProportions.empty()) {
        return UC::Status::InvalidParam("pool.block_proportions must not be empty");
    }
    if (config.poolBlockSizes.size() != config.poolBlockProportions.size()) {
        return UC::Status::InvalidParam(
            "pool.block_sizes and pool.block_proportions must have the same length");
    }
    for (const auto blockSize : config.poolBlockSizes) {
        status = RequirePositive("pool.block_sizes item", blockSize);
        if (status.Failure()) { return status; }
    }
    for (const auto proportion : config.poolBlockProportions) {
        status = RequirePositive("pool.block_proportions item", proportion);
        if (status.Failure()) { return status; }
    }

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
    if (config.pollerMaxPending < config.pollerDrainBudget ||
        config.pollerMaxPending < config.pollerScanBudget) {
        return UC::Status::InvalidParam(
            "poller.max_pending must be greater than or equal to poller budgets");
    }
    status = RequirePositive("op.timeout_ms", config.opTimeoutMs);
    if (status.Failure()) { return status; }
    status = RequirePositive("shutdown.timeout_ms", config.shutdownTimeoutMs);
    if (status.Failure()) { return status; }
    if (config.gcEnabled) {
        status = RequirePositive("gc.interval_ms", config.gcIntervalMs);
        if (status.Failure()) { return status; }
    }

    const auto logLevel = ToLower(config.logLevel);
    if (logLevel != "debug" && logLevel != "info" && logLevel != "warn" &&
        logLevel != "error" && logLevel != "critical") {
        return UC::Status::InvalidParam("unsupported log.level: {}", config.logLevel);
    }
    if (Trim(config.logDir).empty()) { return UC::Status::InvalidParam("log.dir is required"); }
    return UC::Status::OK();
}

}  // namespace UC::DRAMPOOL
