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
    TimedOut = 1,
};

struct InflightRecord {
    // One async handle owns one request batch and its sole response.
    KvOpcode opcode{KvOpcode::None};
    TransportHandle handle{transport::kInvalidTransferHandle};
    std::uint64_t response_addr{0};
    transport::ManagerID peer_manager_id;
    std::vector<std::uint32_t> results;
    std::vector<TransferItem> transfer_items;
    std::uint64_t submit_ms{0};
    InflightPhase phase{InflightPhase::Polling};
};

using TransHandleQueue = UC::SpscRingQueue<InflightRecord>;

class MetadataIndex;

using BufferManagerList = std::vector<std::unique_ptr<UC::ASU::BufferManager>>;

// Non-owning runtime view. DramPoolServer owns every referenced component.
struct DramPoolRuntime {
    DramPoolRuntime(MetadataIndex& metadataRef, BufferManagerList& bufferManagersRef,
                    transport::TransportManager& transportRef, ProtocolManager& protocolRef,
                    RequestQueue& requestQueueRef, TransHandleQueue& transHandleQueueRef)
        : metadata(metadataRef),
          bufferManagers(bufferManagersRef),
          transport(transportRef),
          protocol(protocolRef),
          requestQueue(requestQueueRef),
          transHandleQueue(transHandleQueueRef)
    {
    }

    MetadataIndex& metadata;
    BufferManagerList& bufferManagers;
    transport::TransportManager& transport;
    ProtocolManager& protocol;
    RequestQueue& requestQueue;
    TransHandleQueue& transHandleQueue;
};

}  // namespace UC::DRAMPOOL
