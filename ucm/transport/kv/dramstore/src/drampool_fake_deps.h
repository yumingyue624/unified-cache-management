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
 * DEALINGS IN THE SOFTWARE.
 * */
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "kv_protocol.h"
#include "status/status.h"

namespace UC::DRAMPOOL {

// Temporary contracts for modules that are not landed yet. Replace these
// definitions with the real BufferMgr / TransportMgr / MetadataIndex headers
// when those modules become available.
using Key = std::array<std::uint8_t, UC::DRAMPOOL::kKvKeySize>;

struct BufferHandle {
    std::uint64_t value{0};

    bool Valid() const noexcept { return value != 0; }
};

struct TransportHandle {
    std::uint64_t value{0};

    bool Valid() const noexcept { return value != 0; }
};

enum class TransportDirection {
    ReadRemoteToLocal = 0,
    WriteLocalToRemote = 1,
};

struct TransportSegment {
    std::uint64_t local_addr{0};
    std::uint64_t remote_addr{0};
    std::uint32_t len{0};
};

struct TransportOp {
    UC::DRAMPOOL::KvOpcode opcode{UC::DRAMPOOL::KvOpcode::None};
    TransportDirection direction{TransportDirection::ReadRemoteToLocal};
    std::vector<TransportSegment> segments;
};

struct BufferSlot {
    BufferHandle handle;
    std::uint64_t addr{0};
    std::uint32_t len{0};
    std::uint32_t class_id{0};
};

struct EntryCreateOptions {
    Key key{};
    BufferHandle buffer_handle;
    std::uint64_t local_addr{0};
    std::uint32_t len{0};
    std::uint64_t abs_pos{0};
    std::uint64_t expire_at_ms{0};
    std::uint32_t class_id{0};
};

enum class ReserveDumpCode {
    Reserved = 0,
    Exists = 1,
    Failed = 2,
};

struct ReserveDumpResult {
    ReserveDumpCode code{ReserveDumpCode::Failed};
    UC::Status status{UC::Status::OK()};
};

enum class LoadPinCode {
    Pinned = 0,
    NotFound = 1,
    NotReady = 2,
    Expired = 3,
    Failed = 4,
};

struct LoadPinResult {
    LoadPinCode code{LoadPinCode::Failed};
    UC::Status status{UC::Status::OK()};
    BufferHandle buffer_handle;
    std::uint64_t local_addr{0};
    std::uint32_t len{0};
};

enum class LookupCode {
    Ready = 0,
    NotFound = 1,
    NotReady = 2,
    Expired = 3,
    Failed = 4,
};

class MetadataIndex {
public:
    virtual ~MetadataIndex() = default;

    virtual bool Contains(const Key& key) = 0;
    virtual ReserveDumpResult ReserveDumpEntry(const EntryCreateOptions& options) = 0;
    virtual void RemoveReserved(const Key& key) = 0;

    virtual LoadPinResult LookupAndPinLoad(const Key& key, std::uint64_t now_ms) = 0;
    virtual void UnpinLoad(const Key& key) = 0;

    virtual LookupCode LookupReady(const Key& key, std::uint64_t now_ms) = 0;
};

class BufferManager {
public:
    virtual ~BufferManager() = default;

    virtual UC::Expected<BufferSlot> Allocate(std::uint32_t len) = 0;
    virtual void Free(BufferHandle handle) = 0;
};

class TransportManager {
public:
    virtual ~TransportManager() = default;

    virtual UC::Expected<TransportHandle> SubmitAsync(const TransportOp& op) = 0;
    virtual UC::Status Cancel(TransportHandle handle) = 0;
};

class ResponseWriter {
public:
    virtual ~ResponseWriter() = default;

    virtual UC::Status WriteResponse(UC::DRAMPOOL::KvOpcode opcode, std::uint64_t resp_addr,
                                 const std::vector<std::uint32_t>& results) = 0;
};

}  // namespace UC::DRAMPOOL
