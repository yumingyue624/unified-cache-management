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
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace UC::DRAMPOOL {
namespace {

struct YamlSection {
    std::size_t indent{0};
    std::string key;
};

constexpr const char* kRequiredRuntimeConfigKeys[] = {
    "transport.manager_addr",
    "transport.local_engine",
    "transport.device_id",
    "queue.request_depth",
    "queue.handle_depth",
    "request_receiver.idle_wait_us",
    "poller.drain_budget",
    "poller.scan_budget",
    "poller.max_pending",
    "poller.idle_wait_us",
    "gc.enabled",
    "gc.interval_ms",
    "operation.timeout_ms",
    "logger.level",
    "logger.dir",
    "logger.max_files",
    "logger.max_size_mb",
};

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
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::string StripYamlComment(const std::string& line)
{
    char quote = '\0';
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (quote != '\0') {
            if (character == quote) { quote = '\0'; }
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == '#') {
            return line.substr(0, index);
        }
    }
    return line;
}

UC::Status ParseYamlScalar(const std::string& key, std::string value, std::string& output)
{
    value = Trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    } else if ((!value.empty() && (value.front() == '"' || value.front() == '\'')) ||
               (!value.empty() && (value.back() == '"' || value.back() == '\''))) {
        return UC::Status::InvalidParam("unmatched quote in YAML key {}", key);
    }
    output = std::move(value);
    return UC::Status::OK();
}

UC::Status ParseUint32Value(const std::string& key, const std::string& value,
                            std::uint32_t& output)
{
    try {
        if (value.empty() || value.front() == '-') {
            throw std::invalid_argument("expected an unsigned integer");
        }
        std::size_t parsed = 0;
        const auto number = std::stoull(value, &parsed, 0);
        if (parsed != value.size() || number > std::numeric_limits<std::uint32_t>::max()) {
            throw std::out_of_range("outside uint32 range");
        }
        output = static_cast<std::uint32_t>(number);
    } catch (const std::exception& error) {
        return UC::Status::InvalidParam("invalid YAML value for {}: {}", key, error.what());
    }
    return UC::Status::OK();
}

UC::Status ParseInt32Value(const std::string& key, const std::string& value,
                           std::int32_t& output)
{
    try {
        std::size_t parsed = 0;
        const auto number = std::stoll(value, &parsed, 0);
        if (parsed != value.size() || number < std::numeric_limits<std::int32_t>::min() ||
            number > std::numeric_limits<std::int32_t>::max()) {
            throw std::out_of_range("outside int32 range");
        }
        output = static_cast<std::int32_t>(number);
    } catch (const std::exception& error) {
        return UC::Status::InvalidParam("invalid YAML value for {}: {}", key, error.what());
    }
    return UC::Status::OK();
}

UC::Status ParseBoolValue(const std::string& key, const std::string& value, bool& output)
{
    const auto normalized = ToLower(value);
    if (normalized == "true" || normalized == "yes" || normalized == "on" || normalized == "1") {
        output = true;
        return UC::Status::OK();
    }
    if (normalized == "false" || normalized == "no" || normalized == "off" || normalized == "0") {
        output = false;
        return UC::Status::OK();
    }
    return UC::Status::InvalidParam("invalid YAML boolean for {}", key);
}

UC::Status ApplyRuntimeConfigValue(DramPoolConfig& config, const std::string& key,
                                   const std::string& value)
{
    if (key == "transport.manager_addr" || key == "transport.local_engine") {
        transport::Endpoint endpoint;
        const auto status = ParseDramPoolEndpoint(key, value, endpoint);
        if (status.Failure()) { return status; }
        if (key == "transport.manager_addr") {
            config.transportManagerEndpoint = std::move(endpoint);
        } else {
            config.hixlEngineEndpoint = std::move(endpoint);
        }
        return UC::Status::OK();
    }
    if (key == "transport.device_id") {
        return ParseInt32Value(key, value, config.transportDeviceId);
    }
    if (key == "queue.request_depth") {
        return ParseUint32Value(key, value, config.requestQueueDepth);
    }
    if (key == "queue.handle_depth") {
        return ParseUint32Value(key, value, config.handleQueueDepth);
    }
    if (key == "request_receiver.idle_wait_us") {
        return ParseUint32Value(key, value, config.requestReceiverIdleWaitUs);
    }
    if (key == "poller.drain_budget") {
        return ParseUint32Value(key, value, config.pollerDrainBudget);
    }
    if (key == "poller.scan_budget") {
        return ParseUint32Value(key, value, config.pollerScanBudget);
    }
    if (key == "poller.max_pending") {
        return ParseUint32Value(key, value, config.pollerMaxPending);
    }
    if (key == "poller.idle_wait_us") {
        return ParseUint32Value(key, value, config.pollerIdleWaitUs);
    }
    if (key == "gc.enabled") { return ParseBoolValue(key, value, config.gcEnabled); }
    if (key == "gc.interval_ms") {
        return ParseUint32Value(key, value, config.gcIntervalMs);
    }
    if (key == "operation.timeout_ms") {
        return ParseUint32Value(key, value, config.opTimeoutMs);
    }
    if (key == "logger.level") {
        config.logLevel = ToLower(value);
        return UC::Status::OK();
    }
    if (key == "logger.dir") {
        config.logDir = value;
        return UC::Status::OK();
    }
    if (key == "logger.max_files") {
        return ParseUint32Value(key, value, config.logMaxFiles);
    }
    if (key == "logger.max_size_mb") {
        return ParseUint32Value(key, value, config.logMaxSizeMb);
    }
    return UC::Status::InvalidParam("unknown DramPool runtime YAML key: {}", key);
}

UC::Status ValidateRuntimeConfig(const DramPoolConfig& config)
{
    if (config.transportManagerEndpoint.host.empty() || config.transportManagerEndpoint.port == 0) {
        return UC::Status::InvalidParam("transport.manager_addr must be <IP>:<PORT>");
    }
    if (config.hixlEngineEndpoint.host.empty() || config.hixlEngineEndpoint.port == 0) {
        return UC::Status::InvalidParam("transport.local_engine must be <IP>:<PORT>");
    }
    if (config.addr.host == config.transportManagerEndpoint.host &&
        config.addr.port == config.transportManagerEndpoint.port) {
        return UC::Status::InvalidParam("--addr and transport.manager_addr must differ");
    }
    if (config.transportDeviceId < 0) {
        return UC::Status::InvalidParam("transport.device_id must not be negative");
    }
    if (config.requestQueueDepth < 2 || config.handleQueueDepth < 2) {
        return UC::Status::InvalidParam("queue depths must be at least 2");
    }
    if (config.requestReceiverIdleWaitUs == 0) {
        return UC::Status::InvalidParam(
            "request_receiver.idle_wait_us must be greater than zero");
    }
    if (config.pollerDrainBudget == 0 || config.pollerScanBudget == 0 ||
        config.pollerMaxPending == 0 || config.pollerIdleWaitUs == 0) {
        return UC::Status::InvalidParam("poller values must be greater than zero");
    }
    if (config.pollerMaxPending < config.pollerDrainBudget ||
        config.pollerMaxPending < config.pollerScanBudget) {
        return UC::Status::InvalidParam("poller.max_pending must cover both poller budgets");
    }
    if (config.gcEnabled && config.gcIntervalMs == 0) {
        return UC::Status::InvalidParam("gc.interval_ms must be greater than zero when GC is enabled");
    }
    if (config.opTimeoutMs == 0) {
        return UC::Status::InvalidParam("operation.timeout_ms must be greater than zero");
    }
    if (config.logLevel != "trace" && config.logLevel != "debug" &&
        config.logLevel != "info" && config.logLevel != "warn" &&
        config.logLevel != "error" && config.logLevel != "critical") {
        return UC::Status::InvalidParam("unsupported logger.level: {}", config.logLevel);
    }
    if (Trim(config.logDir).empty()) {
        return UC::Status::InvalidParam("logger.dir must not be empty");
    }
    if (config.logMaxFiles == 0 || config.logMaxSizeMb == 0 ||
        config.logMaxFiles > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        config.logMaxSizeMb > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return UC::Status::InvalidParam("logger rotation values must fit positive int values");
    }
    return UC::Status::OK();
}

UC::Status ValidateRequiredRuntimeConfigKeys(
    const std::unordered_set<std::string>& configuredKeys)
{
    for (const char* key : kRequiredRuntimeConfigKeys) {
        if (configuredKeys.find(key) == configuredKeys.end()) {
            return UC::Status::InvalidParam("DramPool runtime YAML is missing required key: {}",
                                            key);
        }
    }
    return UC::Status::OK();
}

}  // namespace

UC::Status LoadDramPoolRuntimeConfig(const std::string& path, DramPoolConfig& config)
{
    std::ifstream input{path};
    if (!input.is_open()) {
        return UC::Status::Error("failed to open DramPool runtime YAML, path=" + path);
    }

    // Keep the caller's launch configuration intact if YAML parsing fails.
    DramPoolConfig loadedConfig = config;
    std::vector<YamlSection> sections;
    std::unordered_set<std::string> configuredKeys;
    std::string line;
    std::uint32_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        line = StripYamlComment(line);
        if (Trim(line).empty()) { continue; }
        if (line.find('\t') != std::string::npos) {
            return UC::Status::InvalidParam("YAML line {} uses a tab for indentation", lineNumber);
        }

        const auto firstContent = line.find_first_not_of(' ');
        const auto indent = firstContent == std::string::npos ? 0 : firstContent;
        const auto content = Trim(line.substr(indent));
        const auto separator = content.find(':');
        if (separator == std::string::npos || separator == 0) {
            return UC::Status::InvalidParam("invalid YAML mapping at line {}", lineNumber);
        }

        const auto key = Trim(content.substr(0, separator));
        if (key.empty() || key.find_first_of(" \t") != std::string::npos) {
            return UC::Status::InvalidParam("invalid YAML key at line {}", lineNumber);
        }
        std::string value;
        auto status = ParseYamlScalar(key, content.substr(separator + 1), value);
        if (status.Failure()) { return status; }

        while (!sections.empty() && sections.back().indent >= indent) { sections.pop_back(); }
        if (value.empty()) {
            sections.push_back(YamlSection{indent, key});
            continue;
        }

        std::string fullKey;
        for (const auto& section : sections) {
            if (!fullKey.empty()) { fullKey += '.'; }
            fullKey += section.key;
        }
        if (!fullKey.empty()) { fullKey += '.'; }
        fullKey += key;
        if (!configuredKeys.insert(fullKey).second) {
            return UC::Status::InvalidParam("duplicate YAML key: {}", fullKey);
        }
        status = ApplyRuntimeConfigValue(loadedConfig, fullKey, value);
        if (status.Failure()) { return status; }
    }
    if (const auto status = ValidateRequiredRuntimeConfigKeys(configuredKeys); status.Failure()) {
        return status;
    }
    if (const auto status = ValidateRuntimeConfig(loadedConfig); status.Failure()) {
        return status;
    }
    config = std::move(loadedConfig);
    return UC::Status::OK();
}

}  // namespace UC::DRAMPOOL
