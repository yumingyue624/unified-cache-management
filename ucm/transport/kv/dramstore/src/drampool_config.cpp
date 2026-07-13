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
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace UC::DRAMPOOL {
namespace {

// Must match the fixed slot layout used by BufferManager.
constexpr std::size_t kSlotAlignment = 64;

bool IsLongOption(const std::string& value)
{ return value.size() > 2 && value.rfind("--", 0) == 0; }

std::string Trim(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) { return ""; }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
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

UC::Status ValidateAddress(const std::string& addr)
{
    return Trim(addr).empty() ? UC::Status::InvalidParam("--addr is required")
                              : UC::Status::OK();
}

UC::Status ValidateNics(const std::vector<std::string>& nics)
{
    for (const auto& nic : nics) {
        if (Trim(nic).empty()) {
            return UC::Status::InvalidParam("--nics contains an empty name");
        }
    }
    return UC::Status::OK();
}

UC::Status ValidatePoolSize(std::uint64_t poolSizeGb)
{
    if (poolSizeGb == 0) {
        return UC::Status::InvalidParam("--pool-size-gb must be greater than zero");
    }
    if (poolSizeGb > std::numeric_limits<std::uint64_t>::max() / kBytesPerGiB) {
        return UC::Status::InvalidParam("--pool-size-gb is too large");
    }
    const auto totalBytes = poolSizeGb * kBytesPerGiB;
    if (static_cast<std::uint64_t>(static_cast<std::size_t>(totalBytes)) != totalBytes) {
        return UC::Status::InvalidParam("--pool-size-gb exceeds addressable process memory");
    }
    return UC::Status::OK();
}

UC::Status ValidateBlockSizes(const std::vector<std::uint64_t>& blockSizes)
{
    for (const auto blockSize : blockSizes) {
        const auto slotCapacity = static_cast<std::size_t>(blockSize);
        if (blockSize == 0 || blockSize > std::numeric_limits<std::uint32_t>::max() ||
            static_cast<std::uint64_t>(slotCapacity) != blockSize) {
            return UC::Status::InvalidParam("--kvcache-block-sizes item is unsupported");
        }
        if (slotCapacity > std::numeric_limits<std::size_t>::max() -
                               (kSlotAlignment - 1)) {
            return UC::Status::InvalidParam("--kvcache-block-sizes item overflows slot alignment");
        }
    }
    return UC::Status::OK();
}

UC::Status ValidateBlockProportions(const std::vector<std::uint32_t>& proportions)
{
    std::uint64_t totalProportion = 0;
    for (const auto proportion : proportions) {
        if (proportion == 0 ||
            totalProportion > std::numeric_limits<std::uint32_t>::max() - proportion) {
            return UC::Status::InvalidParam(
                "sum of --kvcache-block-proportions must be in [1, {}]",
                std::numeric_limits<std::uint32_t>::max());
        }
        totalProportion += proportion;
    }
    return UC::Status::OK();
}

UC::Status ValidateBlockClassCount(const std::vector<std::uint64_t>& blockSizes,
                                   const std::vector<std::uint32_t>& proportions)
{
    if (blockSizes.size() != proportions.size()) {
        return UC::Status::InvalidParam(
            "--kvcache-block-sizes and --kvcache-block-proportions must have the same length");
    }
    return UC::Status::OK();
}

UC::Status CalculatePoolSlotCounts(DramPoolConfig& config)
{
    config.poolSlotCounts.clear();
    std::uint64_t totalProportion = 0;
    for (const auto proportion : config.poolBlockProportions) { totalProportion += proportion; }

    const auto totalBytes = config.poolSizeGb * kBytesPerGiB;
    std::vector<std::uint32_t> slotCounts;
    slotCounts.reserve(config.poolBlockSizes.size());
    const auto wholeShare = totalBytes / totalProportion;
    const auto remainder = totalBytes % totalProportion;
    for (std::size_t index = 0; index < config.poolBlockSizes.size(); ++index) {
        const auto slotCapacity = static_cast<std::size_t>(config.poolBlockSizes[index]);
        const auto slotStride =
            (slotCapacity + kSlotAlignment - 1) / kSlotAlignment * kSlotAlignment;
        const auto proportion = static_cast<std::uint64_t>(config.poolBlockProportions[index]);
        // totalProportion is bounded to uint32_t, so this remainder product cannot overflow.
        const auto classBytes = wholeShare * proportion + remainder * proportion / totalProportion;
        const auto slotCount = classBytes / slotStride;
        if (slotCount == 0 || slotCount >= std::numeric_limits<std::uint32_t>::max()) {
            return UC::Status::InvalidParam(
                "pool capacity for --kvcache-block-sizes item {} cannot form a valid slot pool",
                index);
        }
        slotCounts.push_back(static_cast<std::uint32_t>(slotCount));
    }

    config.poolSlotCounts = std::move(slotCounts);
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
            status = ValidateAddress(config.addr);
            if (status.Failure()) { return status; }
            hasAddr = true;
            continue;
        }
        if (option == "--nics") {
            if (hasNics) { return UC::Status::InvalidParam("--nics may be specified once"); }
            status = ReadListValues(option, hasInlineValue, inlineValue, argc, argv, index, values);
            if (status.Failure()) { return status; }
            config.nics = std::move(values);
            status = ValidateNics(config.nics);
            if (status.Failure()) { return status; }
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
            status = ValidatePoolSize(config.poolSizeGb);
            if (status.Failure()) { return status; }
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
            status = ValidateBlockSizes(config.poolBlockSizes);
            if (status.Failure()) { return status; }
            hasBlockSizes = true;
            if (hasBlockProportions) {
                status = ValidateBlockClassCount(config.poolBlockSizes,
                                                 config.poolBlockProportions);
                if (status.Failure()) { return status; }
            }
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
            status = ValidateBlockProportions(config.poolBlockProportions);
            if (status.Failure()) { return status; }
            hasBlockProportions = true;
            if (hasBlockSizes) {
                status = ValidateBlockClassCount(config.poolBlockSizes,
                                                 config.poolBlockProportions);
                if (status.Failure()) { return status; }
            }
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
    // Options are order-independent; all pool inputs are complete only here.
    if (const auto status = CalculatePoolSlotCounts(config); status.Failure()) {
        return status;
    }
    return UC::Status::OK();
}

}  // namespace UC::DRAMPOOL
