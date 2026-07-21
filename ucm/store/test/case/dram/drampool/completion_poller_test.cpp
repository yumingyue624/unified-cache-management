#include "completion_poller.h"
#include <acl/acl.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <utility>
#include <vector>
#include "core/transport_manager.h"
#include "drampool_config.h"
#include "metadata.h"
#include "task_worker.h"
#include "test_transport.h"

namespace UC::DramPool {
namespace {

constexpr auto kConditionWaitTimeout = std::chrono::seconds(2);
constexpr auto kConditionPollInterval = std::chrono::milliseconds(1);
constexpr std::uint64_t kTestOperationTimeoutMs = 60'000;
constexpr std::uint32_t kValueLength = 16;
constexpr std::size_t kFlagSlotSize = 64;
constexpr char kTargetManager[] = "127.0.0.1:29000";

std::uint64_t SteadyNowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool WaitUntil(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + kConditionWaitTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) { return true; }
        std::this_thread::sleep_for(kConditionPollInterval);
    }
    return predicate();
}

BlockId MakeKey(std::uint8_t suffix)
{
    BlockId key{};
    key.back() = static_cast<std::byte>(suffix);
    return key;
}

UC::DramPool::MetadataConfig MakeMetadataConfig()
{
    return {
        UC::DramPool::EvictionPolicyType::TTL,
        UC::DramPool::EvictionPolicyType::POSITION,
        std::chrono::milliseconds(100),
        0.0,
    };
}

class CompletionPollerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        savedConfig_ = g_config;
        requestQueue_.Setup(kQueueCapacity);
        ingress_.Setup(kQueueCapacity);
        ASSERT_EQ(manager_.InstallTransport(testTransport_, transport::InitAttrs{}),
                  transport::Status::Ok);

        g_config.pollerPendingDepth = kTestPendingDepth;
        g_config.opTimeoutMs = kTestOperationTimeoutMs;
        g_config.poolBlockSizes = {kValueLength};
        g_config.poolSlotCounts = {static_cast<std::uint32_t>(kQueueCapacity)};

        // BufferPool uses aclrtMallocHost for its HOST allocation.
        const auto aclInitStatus = aclInit(nullptr);
        if (aclInitStatus == ACL_SUCCESS) {
            ownsAclRuntime_ = true;
        } else {
            ASSERT_EQ(aclInitStatus, ACL_ERROR_REPEAT_INITIALIZE);
        }
        ASSERT_EQ(aclrtSetDevice(g_config.transportDeviceId), ACL_SUCCESS);

        bufferManager_ =
            std::make_unique<BufferManager>(std::vector<std::pair<std::size_t, std::size_t>>{
                {kValueLength, kQueueCapacity}
        });
        metadata_ = std::make_unique<MetadataManager>(MakeMetadataConfig(), *bufferManager_);
        ASSERT_TRUE(flagBufferPool_.Init("test_flag_buffer_pool", BufferPool::MemoryType::HOST,
                                         kFlagSlotSize, kQueueCapacity)
                        .Success());

        ASSERT_EQ(manager_.RegisterMemory(bufferManager_->MemoryRegions().front(),
                                          bufferPoolMemoryHandle_),
                  transport::Status::Ok);
        transport::MemoryRegion flagRegion{flagBufferPool_.GetLocalAddr(),
                                           flagBufferPool_.GetTotalSize(),
                                           transport::MemoryType::Host};
        ASSERT_EQ(manager_.RegisterMemory(flagRegion, flagBufferMemoryHandle_),
                  transport::Status::Ok);

        runtime_ = std::make_unique<DramPoolRuntime>(*metadata_, flagBufferPool_, manager_,
                                                     protocols_, requestQueue_, ingress_);
    }

    void TearDown() override
    {
        runtime_.reset();
        metadata_.reset();
        EXPECT_EQ(manager_.UnregisterMemory(flagBufferMemoryHandle_), transport::Status::Ok);
        EXPECT_EQ(manager_.UnregisterMemory(bufferPoolMemoryHandle_), transport::Status::Ok);
        flagBufferPool_.Reset();
        bufferManager_.reset();
        if (ownsAclRuntime_) {
            (void)aclrtResetDevice(g_config.transportDeviceId);
            (void)aclFinalize();
            ownsAclRuntime_ = false;
        }
        g_config = std::move(savedConfig_);
    }

    CompletionRecord MakeDumpRecord(std::uint8_t keySuffix, std::uint64_t responseAddr,
                                    TransportHandle& handleOut)
    {
        auto entry = std::make_shared<UC::DramPool::Entry>();
        entry->key = MakeKey(keySuffix);
        entry->size = kValueLength;
        entry->lifeTimeout = std::chrono::system_clock::now() + std::chrono::hours(1);
        EXPECT_TRUE(metadata_->StoreBegin(entry->key, entry).Success());

        transport::Operation operation;
        operation.opcode = transport::Opcode::Read;
        operation.direct = transport::OperationDirect::RemoteDeviceHost;
        operation.target_manager = kTargetManager;
        operation.ops.push_back(
            transport::Segment{entry->buffer.addr, responseAddr, entry->size});
        EXPECT_EQ(manager_.ExecuteAsync(operation, handleOut), transport::Status::Ok);

        std::vector<TransferItem> items{
            TransferItem{0, entry->key}
        };
        std::vector<std::uint8_t> results{static_cast<std::uint8_t>(ResultCode::Failed)};

        CompletionRecord record;
        record.stage = CompletionStage::DataTransfer;
        record.opcode = KvOpcode::Dump;
        record.data_handle = handleOut;
        record.response_addr = responseAddr;
        record.peer_one_sided_id = kTargetManager;
        record.results = std::move(results);
        record.transfer_items = std::move(items);
        record.submit_ms = SteadyNowMs();
        return record;
    }

    static constexpr std::size_t kQueueCapacity = 16;
    static constexpr std::size_t kTestPendingDepth = 2;

    RequestQueue requestQueue_;
    CompletionQueue ingress_;
    std::unique_ptr<BufferManager> bufferManager_;
    std::unique_ptr<MetadataManager> metadata_;
    BufferPool flagBufferPool_;
    transport::MemoryHandle bufferPoolMemoryHandle_{transport::kInvalidMemoryHandle};
    transport::MemoryHandle flagBufferMemoryHandle_{transport::kInvalidMemoryHandle};
    bool ownsAclRuntime_{false};
    ProtocolManager protocols_;
    std::shared_ptr<TEST::TestTransport> testTransport_{TEST::MakeTestTransport()};
    transport::TransportManager manager_{"127.0.0.1:28000"};
    std::unique_ptr<DramPoolRuntime> runtime_;
    DramPoolConfig savedConfig_;
};

TEST_F(CompletionPollerTest, FlagBufferPoolAllocatesAndReusesSlot)
{
    BufferPool::Slot first;
    ASSERT_TRUE(flagBufferPool_.Allocate(first).Success());
    EXPECT_EQ(first.length, kFlagSlotSize);
    EXPECT_EQ(testTransport_->ActiveMemoryCount(), 2U);
    ASSERT_TRUE(flagBufferPool_.Free(first.slot_index).Success());

    BufferPool::Slot reused;
    ASSERT_TRUE(flagBufferPool_.Allocate(reused).Success());
    EXPECT_EQ(reused.slot_index, first.slot_index);
    EXPECT_TRUE(flagBufferPool_.Free(reused.slot_index).Success());
}

TEST_F(CompletionPollerTest, RefillsConfiguredPendingWindowAfterCompletedRecordsAreRemoved)
{
    TransportHandle firstHandle = transport::kInvalidTransferHandle;
    TransportHandle secondHandle = transport::kInvalidTransferHandle;
    TransportHandle thirdHandle = transport::kInvalidTransferHandle;
    auto first = MakeDumpRecord(1, 101, firstHandle);
    auto second = MakeDumpRecord(2, 102, secondHandle);
    auto third = MakeDumpRecord(3, 103, thirdHandle);

    ASSERT_TRUE(testTransport_->SetStatus(firstHandle, transport::TransferStatus::Waiting));
    ASSERT_TRUE(testTransport_->SetStatus(secondHandle, transport::TransferStatus::Waiting));
    ingress_.Push(std::move(first));
    ingress_.Push(std::move(second));
    ingress_.Push(std::move(third));

    CompletionPoller poller(*runtime_);
    std::atomic_bool stop{false};
    std::thread pollerThread([&]() { poller.Run(stop); });

    ASSERT_TRUE(WaitUntil([&]() {
        return testTransport_->QueryCount(firstHandle) > 0 &&
               testTransport_->QueryCount(secondHandle) > 0;
    }));
    EXPECT_EQ(testTransport_->QueryCount(thirdHandle), 0U);
    EXPECT_EQ(testTransport_->SyncExecutionCount(), 0U);

    EXPECT_TRUE(testTransport_->SetStatus(firstHandle, transport::TransferStatus::Completed));
    EXPECT_TRUE(testTransport_->SetStatus(secondHandle, transport::TransferStatus::Completed));
    const bool allCompleted = WaitUntil([&]() {
        return testTransport_->AsyncExecutionCount() == 6 &&
               testTransport_->ActiveTransferCount() == 0;
    });

    stop.store(true, std::memory_order_release);
    pollerThread.join();

    EXPECT_TRUE(allCompleted);
    EXPECT_EQ(testTransport_->ActiveTransferCount(), 0U);
    EXPECT_TRUE(metadata_->Exist(MakeKey(1)));
    EXPECT_TRUE(metadata_->Exist(MakeKey(2)));
    EXPECT_TRUE(metadata_->Exist(MakeKey(3)));
}

TEST_F(CompletionPollerTest, TimeoutWaitsForTerminalThenAbortsDump)
{
    g_config.pollerPendingDepth = 1;
    g_config.opTimeoutMs = 1;

    TransportHandle handle = transport::kInvalidTransferHandle;
    auto record = MakeDumpRecord(7, 107, handle);
    record.submit_ms = 0;
    ASSERT_TRUE(testTransport_->SetStatus(handle, transport::TransferStatus::Waiting));
    ingress_.Push(std::move(record));

    CompletionPoller poller(*runtime_);
    std::atomic_bool stop{false};
    std::thread pollerThread([&]() { poller.Run(stop); });

    ASSERT_TRUE(WaitUntil([&]() { return testTransport_->QueryCount(handle) > 0; }));
    EXPECT_TRUE(metadata_->Query(MakeKey(7)));
    EXPECT_FALSE(metadata_->Exist(MakeKey(7)));
    EXPECT_EQ(testTransport_->ActiveMemoryCount(), 2U);

    ASSERT_TRUE(testTransport_->SetStatus(handle, transport::TransferStatus::Completed));
    const bool completed = WaitUntil([&]() {
        return testTransport_->AsyncExecutionCount() == 2 &&
               testTransport_->ActiveTransferCount() == 0;
    });
    stop.store(true, std::memory_order_release);
    pollerThread.join();

    EXPECT_TRUE(completed);
    EXPECT_FALSE(metadata_->Query(MakeKey(7)));
    EXPECT_EQ(testTransport_->ActiveMemoryCount(), 2U);
    EXPECT_EQ(testTransport_->ActiveTransferCount(), 0U);
}

TEST_F(CompletionPollerTest, KeepsResponseBufferUntilAsyncWriteReachesTerminal)
{
    g_config.pollerPendingDepth = 1;

    TransportHandle dataHandle = transport::kInvalidTransferHandle;
    auto record = MakeDumpRecord(9, 109, dataHandle);
    testTransport_->SetNewTransferStatus(transport::TransferStatus::Waiting);
    ingress_.Push(std::move(record));

    CompletionPoller poller(*runtime_);
    std::atomic_bool stop{false};
    std::thread pollerThread([&]() { poller.Run(stop); });

    ASSERT_TRUE(WaitUntil([&]() { return testTransport_->AsyncExecutionCount() == 2; }));
    const auto responseHandle = testTransport_->LatestTransferHandle();
    ASSERT_NE(responseHandle, transport::kInvalidTransferHandle);
    EXPECT_EQ(testTransport_->SyncExecutionCount(), 0U);
    EXPECT_EQ(testTransport_->ActiveTransferCount(), 1U);
    // The complete pool remains registered while individual slots change ownership.
    EXPECT_EQ(testTransport_->ActiveMemoryCount(), 2U);
    std::vector<BufferPool::Slot> fillerSlots;
    for (std::size_t index = 0; index < kQueueCapacity - 1; ++index) {
        BufferPool::Slot filler;
        ASSERT_TRUE(flagBufferPool_.Allocate(filler).Success());
        fillerSlots.push_back(filler);
    }
    BufferPool::Slot exhausted;
    EXPECT_EQ(flagBufferPool_.Allocate(exhausted), Status::NoSpace());

    ASSERT_TRUE(testTransport_->SetStatus(responseHandle, transport::TransferStatus::Completed));
    BufferPool::Slot releasedSlot;
    const bool responseReleased =
        WaitUntil([&]() { return flagBufferPool_.Allocate(releasedSlot).Success(); });
    if (responseReleased) { EXPECT_TRUE(flagBufferPool_.Free(releasedSlot.slot_index).Success()); }
    for (const auto& filler : fillerSlots) {
        EXPECT_TRUE(flagBufferPool_.Free(filler.slot_index).Success());
    }

    stop.store(true, std::memory_order_release);
    pollerThread.join();

    EXPECT_TRUE(responseReleased);
    EXPECT_TRUE(metadata_->Exist(MakeKey(9)));
}

TEST_F(CompletionPollerTest, SendsResponseReadyRecordWithoutBlocking)
{
    testTransport_->SetNewTransferStatus(transport::TransferStatus::Waiting);

    CompletionRecord record;
    record.stage = CompletionStage::ResponseReady;
    record.opcode = KvOpcode::Lookup;
    record.response_addr = 110;
    record.peer_one_sided_id = kTargetManager;
    record.results = {1, 0, 1, 1, 0, 0, 0, 1, 1};
    ingress_.Push(std::move(record));

    CompletionPoller poller(*runtime_);
    std::atomic_bool stop{false};
    std::thread pollerThread([&]() { poller.Run(stop); });

    ASSERT_TRUE(WaitUntil([&]() { return testTransport_->AsyncExecutionCount() == 1; }));
    const auto responseHandle = testTransport_->LatestTransferHandle();
    ASSERT_NE(responseHandle, transport::kInvalidTransferHandle);
    EXPECT_EQ(testTransport_->SyncExecutionCount(), 0U);
    EXPECT_EQ(testTransport_->LatestOperationLength(), 3U);
    EXPECT_EQ(testTransport_->ActiveMemoryCount(), 2U);
    std::vector<BufferPool::Slot> fillerSlots;
    for (std::size_t index = 0; index < kQueueCapacity - 1; ++index) {
        BufferPool::Slot filler;
        ASSERT_TRUE(flagBufferPool_.Allocate(filler).Success());
        fillerSlots.push_back(filler);
    }
    BufferPool::Slot exhausted;
    EXPECT_EQ(flagBufferPool_.Allocate(exhausted), Status::NoSpace());

    ASSERT_TRUE(testTransport_->SetStatus(responseHandle, transport::TransferStatus::Completed));
    BufferPool::Slot releasedSlot;
    const bool responseReleased =
        WaitUntil([&]() { return flagBufferPool_.Allocate(releasedSlot).Success(); });
    if (responseReleased) { EXPECT_TRUE(flagBufferPool_.Free(releasedSlot.slot_index).Success()); }
    for (const auto& filler : fillerSlots) {
        EXPECT_TRUE(flagBufferPool_.Free(filler.slot_index).Success());
    }

    stop.store(true, std::memory_order_release);
    pollerThread.join();

    EXPECT_TRUE(responseReleased);
}

TEST_F(CompletionPollerTest, LookupReturnsOneResultForEveryKey)
{
    const auto publish = [this](std::uint8_t suffix) {
        auto entry = std::make_shared<UC::DramPool::Entry>();
        entry->key = MakeKey(suffix);
        entry->size = kValueLength;
        entry->lifeTimeout = std::chrono::system_clock::now() + std::chrono::hours(1);
        ASSERT_TRUE(metadata_->StoreBegin(entry->key, entry).Success());
        ASSERT_TRUE(metadata_->StoreEnd(entry->key).Success());
    };
    publish(1);
    publish(3);

    auto request = std::make_unique<KvLookupRequest>();
    request->opcode = KvOpcode::Lookup;
    request->resp_addr = 120;
    request->batch_size = 3;
    request->entries.resize(3);
    for (std::uint8_t index = 0; index < 3; ++index) {
        const auto key = MakeKey(static_cast<std::uint8_t>(index + 1));
        std::memcpy(request->entries[index].key.data(), key.data(), key.size());
    }

    auto task = std::make_unique<RequestTask>();
    task->request = std::move(request);
    task->peer_one_sided_id = kTargetManager;
    requestQueue_.Push(std::move(task));

    TaskWorker worker(*runtime_);
    std::atomic_bool stop{false};
    std::thread workerThread([&]() { worker.Run(stop); });

    CompletionRecord record;
    const bool completed = WaitUntil([&]() { return ingress_.TryPop(record); });
    stop.store(true, std::memory_order_release);
    workerThread.join();

    ASSERT_TRUE(completed);
    EXPECT_EQ(record.stage, CompletionStage::ResponseReady);
    EXPECT_EQ(record.opcode, KvOpcode::Lookup);
    EXPECT_EQ(record.results, (std::vector<std::uint8_t>{1, 0, 1}));
}

}  // namespace
}  // namespace UC::DramPool
