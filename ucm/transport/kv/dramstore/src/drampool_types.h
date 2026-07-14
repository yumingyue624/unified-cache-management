/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "core/transport.h"
#include "kv_protocol.h"
#include "status/status.h"
#include "template/spsc_ring_queue.h"
#include "type/types.h"

namespace transport {
class TransportManager;
}

namespace UC::ASU {
class BufferManager;
}

namespace UC::DRAMPOOL {

using BlockId = UC::Detail::BlockId;
using RequestPtr = std::unique_ptr<KvRequest>;

// Receiver keeps transport identity beside the parsed KV request.
struct RequestTask {
    RequestPtr request;
    transport::ManagerID peer_manager_id;
};

using RequestTaskPtr = std::unique_ptr<RequestTask>;
using RequestQueue = UC::SpscRingQueue<RequestTaskPtr>;

struct BufferHandle {
    std::uint64_t value{0};
    std::uint32_t class_id{0};
    transport::MemoryHandle memory_handle{transport::kInvalidMemoryHandle};

    bool Valid() const noexcept { return value != 0; }
};

struct BufferSlot {
    BufferHandle handle;
    std::uint64_t addr{0};
    std::uint32_t len{0};
    std::uint32_t class_id{0};
};

using TransportHandle = transport::TransferHandle;

inline UC::Status ToUcStatus(transport::Status status, const char* operation)
{
    if (status == transport::Status::Ok) { return UC::Status::OK(); }
    if (status == transport::Status::InvalidArgument) {
        return UC::Status::InvalidParam("{}: invalid transport argument", operation);
    }
    return UC::Status::Error(std::string{operation} + ": transport operation failed");
}

struct TransferItem {
    // State needed to settle one item after the batch reaches terminal.
    std::uint16_t index_in_request{0};
    BlockId key{};
    BufferHandle buffer_handle;
};

enum class ResultCode : std::uint32_t {
    Ok = 0,
    Failed = 1,
};

enum class InflightPhase : std::uint8_t {
    Polling = 0,
    // The transfer must still reach terminal, but its request result is forced to failure.
    FailurePending = 1,
};

enum class CompletionStage : std::uint8_t {
    DataTransfer = 0,
    ResponseReady = 1,
    ResponseTransfer = 2,
};

struct CompletionRecord {
    CompletionStage stage{CompletionStage::DataTransfer};

    // State used while the request's data transfer is in flight.
    TransportHandle data_handle{transport::kInvalidTransferHandle};
    std::vector<TransferItem> transfer_items;
    std::uint64_t submit_ms{0};
    InflightPhase phase{InflightPhase::Polling};

    // State needed to construct the request's sole response.
    KvOpcode opcode{KvOpcode::None};
    std::uint64_t response_addr{0};
    transport::ManagerID peer_manager_id;
    std::vector<std::uint32_t> results;

    // Ownership held from response submission until its handle reaches terminal.
    TransportHandle response_handle{transport::kInvalidTransferHandle};
    BufferHandle response_buffer;
};

using CompletionQueue = UC::SpscRingQueue<CompletionRecord>;

class MetadataIndex;

using BufferManagerList = std::vector<std::unique_ptr<UC::ASU::BufferManager>>;

// Non-owning runtime view. DramPoolServer owns every referenced component.
struct DramPoolRuntime {
    DramPoolRuntime(MetadataIndex& metadataRef, BufferManagerList& bufferManagersRef,
                    transport::TransportManager& transportRef, ProtocolManager& protocolRef,
                    RequestQueue& requestQueueRef, CompletionQueue& completionQueueRef)
        : metadata(metadataRef),
          bufferManagers(bufferManagersRef),
          transport(transportRef),
          protocol(protocolRef),
          requestQueue(requestQueueRef),
          completionQueue(completionQueueRef)
    {
    }

    MetadataIndex& metadata;
    BufferManagerList& bufferManagers;
    transport::TransportManager& transport;
    ProtocolManager& protocol;
    RequestQueue& requestQueue;
    CompletionQueue& completionQueue;
};

}  // namespace UC::DRAMPOOL
