#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "core/transport.h"

namespace UC::DRAMPOOL::TEST {

class TestTransport final : public transport::Transport {
public:
    transport::TransportProtocol Protocol() const override
    { return transport::TransportProtocol::Hixl; }

    transport::Status Init(const transport::InitAttrs&) override { return transport::Status::Ok; }

    transport::Status Shutdown() override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        transfers_.clear();
        return transport::Status::Ok;
    }

    transport::Status RegisterMemory(const transport::MemoryRegion& memory,
                                     transport::MemoryHandle& handle) override
    {
        if (memory.addr == nullptr || memory.length == 0) {
            return transport::Status::InvalidArgument;
        }
        std::lock_guard<std::mutex> guard(mutex_);
        handle = nextMemoryHandle_++;
        memories_.emplace(handle, memory);
        return transport::Status::Ok;
    }

    transport::Status UnregisterMemory(transport::MemoryHandle handle) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return memories_.erase(handle) == 1 ? transport::Status::Ok : transport::Status::Failed;
    }

    transport::Status ExportMetadata(const transport::ManagerID&, transport::Metadata& out) override
    {
        out = {1};
        return transport::Status::Ok;
    }

    transport::Status ImportMetadata(const transport::ManagerID&,
                                     const transport::Metadata&) override
    { return transport::Status::Ok; }

    transport::Status Connect(const transport::ManagerID&) override
    { return transport::Status::Ok; }

    transport::Status Disconnect(const transport::ManagerID&) override
    { return transport::Status::Ok; }

    transport::Status ExecuteSync(const transport::Operation& operation) override
    {
        if (operation.target_manager.empty() || operation.ops.empty()) {
            return transport::Status::InvalidArgument;
        }
        std::lock_guard<std::mutex> guard(mutex_);
        ++syncExecutionCount_;
        return transport::Status::Ok;
    }

    transport::Status ExecuteAsync(const transport::Operation& operation,
                                   transport::TransferHandle& handle) override
    {
        if (operation.target_manager.empty() || operation.ops.empty()) {
            return transport::Status::InvalidArgument;
        }
        std::lock_guard<std::mutex> guard(mutex_);
        handle = nextTransferHandle_++;
        transfers_.emplace(handle, transport::TransferStatus::Completed);
        return transport::Status::Ok;
    }

    transport::Status GetStatus(transport::TransferHandle handle,
                                transport::TransferStatus& status) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto iter = transfers_.find(handle);
        if (iter == transfers_.end()) { return transport::Status::Failed; }
        ++queryCounts_[handle];
        status = iter->second;
        if (status != transport::TransferStatus::Waiting) { transfers_.erase(iter); }
        return transport::Status::Ok;
    }

    bool SetStatus(transport::TransferHandle handle, transport::TransferStatus status)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto iter = transfers_.find(handle);
        if (iter == transfers_.end()) { return false; }
        iter->second = status;
        return true;
    }

    std::size_t QueryCount(transport::TransferHandle handle) const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto iter = queryCounts_.find(handle);
        return iter == queryCounts_.end() ? 0 : iter->second;
    }

    std::size_t ActiveTransferCount() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return transfers_.size();
    }

    std::size_t SyncExecutionCount() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return syncExecutionCount_;
    }

private:
    mutable std::mutex mutex_;
    transport::MemoryHandle nextMemoryHandle_{1};
    transport::TransferHandle nextTransferHandle_{1};
    std::unordered_map<transport::MemoryHandle, transport::MemoryRegion> memories_;
    std::unordered_map<transport::TransferHandle, transport::TransferStatus> transfers_;
    std::unordered_map<transport::TransferHandle, std::size_t> queryCounts_;
    std::size_t syncExecutionCount_{0};
};

inline std::shared_ptr<TestTransport> MakeTestTransport()
{ return std::make_shared<TestTransport>(); }

}  // namespace UC::DRAMPOOL::TEST
