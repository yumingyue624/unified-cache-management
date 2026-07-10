/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>
#include "kv_protocol.h"
#include "status/status.h"
#include "template/spsc_ring_queue.h"
#include "type/types.h"

namespace UC::DRAMPOOL {

using BlockId = UC::Detail::BlockId;
using RequestPtr = std::unique_ptr<KvRequest>;
using RequestQueue = UC::SpscRingQueue<RequestPtr>;

struct BufferHandle {
    std::uint64_t value{0};
    std::uint32_t class_id{0};

    bool Valid() const noexcept { return value != 0; }
};

struct TransportHandle {
    std::uint64_t value{0};

    bool Valid() const noexcept { return value != 0; }
};

struct TransferItem {
    std::uint16_t request_index{0};
    BlockId key{};
    std::uint64_t remote_addr{0};
    std::uint32_t len{0};
    BufferHandle buffer_handle;
};

enum class ResultCode : std::uint32_t {
    Ok = 0,
    Failed = 1,
};

inline constexpr std::uint32_t ToResultValue(ResultCode code) noexcept
{ return static_cast<std::uint32_t>(code); }

struct FinalResponse {
    KvOpcode opcode{KvOpcode::None};
    std::uint64_t response_addr{0};
    std::vector<std::uint32_t> results;
};

// Tracks one batch across asynchronous completions and claims its response once.
class RequestContext final {
public:
    RequestContext(KvOpcode opcode, std::uint64_t responseAddr,
                   std::vector<std::uint32_t> initialResults,
                   const std::vector<TransferItem>& pendingItems)
        : opcode_(opcode),
          responseAddr_(responseAddr),
          results_(std::move(initialResults)),
          completed_(results_.size(), true)
    {
        for (const auto& item : pendingItems) {
            if (item.request_index >= completed_.size() || !completed_[item.request_index]) {
                valid_ = false;
                continue;
            }
            completed_[item.request_index] = false;
            ++remaining_;
        }
    }

    bool Valid() const noexcept { return valid_; }

    UC::Status CompleteItem(std::uint16_t requestIndex, ResultCode result,
                            std::optional<FinalResponse>& finalResponse)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!valid_) { return UC::Status::InvalidParam("invalid request context"); }
        if (requestIndex >= completed_.size()) {
            return UC::Status::InvalidParam("request result index out of range");
        }
        if (completed_[requestIndex]) { return UC::Status::DuplicateKey(); }

        completed_[requestIndex] = true;
        results_[requestIndex] = ToResultValue(result);
        --remaining_;

        if (remaining_ == 0 && !responseClaimed_) {
            responseClaimed_ = true;
            finalResponse = FinalResponse{opcode_, responseAddr_, results_};
        }
        return UC::Status::OK();
    }

private:
    KvOpcode opcode_{KvOpcode::None};
    std::uint64_t responseAddr_{0};
    std::vector<std::uint32_t> results_;
    std::vector<std::uint8_t> completed_;
    std::size_t remaining_{0};
    bool responseClaimed_{false};
    bool valid_{true};
    std::mutex mutex_;
};

enum class InflightPhase : std::uint8_t {
    Polling = 0,
    CancelRequested = 1,
    // Business state is settled; only transport-handle release may be retried.
    AppliedAwaitingRelease = 2,
};

struct InflightRecord {
    KvOpcode opcode{KvOpcode::None};
    TransportHandle handle;
    std::vector<TransferItem> transfer_items;
    std::shared_ptr<RequestContext> request_ctx;
    std::uint64_t submit_ms{0};
    InflightPhase phase{InflightPhase::Polling};
};

using TransHandleQueue = UC::SpscRingQueue<InflightRecord>;

}  // namespace UC::DRAMPOOL
