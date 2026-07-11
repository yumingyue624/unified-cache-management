#include "completion_poller.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <thread>
#include <utility>
#include <vector>
#include "drampool_config.h"
#include "test_transport.h"

namespace UC::DRAMPOOL {
namespace {

constexpr auto kConditionWaitTimeout = std::chrono::seconds(2);
constexpr auto kConditionPollInterval = std::chrono::milliseconds(1);
constexpr std::uint64_t kTestOperationTimeoutMs = 60'000;
constexpr std::uint32_t kTestIdleWaitUs = 50;
constexpr std::uint32_t kValueLength = 16;
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

class CompletionPollerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ingress_.Setup(kQueueCapacity);
        ASSERT_EQ(manager_.InstallTransport(testTransport_, transport::InitAttrs{}),
                  transport::Status::Ok);

        g_config.pollerDrainBudget = kDrainBudget;
        g_config.pollerScanBudget = kHeadScanBudget;
        g_config.pollerMaxPending = kQueueCapacity;
        g_config.pollerIdleWaitUs = kTestIdleWaitUs;
        g_config.opTimeoutMs = kTestOperationTimeoutMs;

        g_services.metadata = &metadata_;
        g_services.buffer_managers = {&buffers_};
        g_services.transport = &manager_;
        g_services.protocol_mgr = &protocols_;
        g_services.trans_handle_queue = &ingress_;
    }

    void TearDown() override { g_services = {}; }

    InflightRecord MakeDumpRecord(std::uint8_t keySuffix, std::uint64_t responseAddr,
                                  TransportHandle& handleOut)
    {
        auto allocated = buffers_.Allocate(kValueLength);
        EXPECT_TRUE(allocated.HasValue());
        auto slot = std::move(allocated).Value();

        auto entry = std::make_shared<UC::DramStore::Entry>();
        entry->key = MakeKey(keySuffix);
        entry->shard = slot.class_id;
        entry->slot = static_cast<std::uint32_t>(slot.handle.value);
        entry->addr = reinterpret_cast<void*>(slot.addr);
        entry->size = slot.len;
        entry->lifeTimeout = std::chrono::system_clock::now() + std::chrono::hours(1);
        EXPECT_EQ(metadata_.ReserveDumpEntry(entry, slot.handle).code,
                  ReserveDumpCode::Reserved);

        transport::Operation operation;
        operation.opcode = transport::Opcode::Read;
        operation.direct = transport::OperationDirect::RemoteDeviceHost;
        operation.target_manager = kTargetManager;
        operation.ops.push_back(
            transport::Segment{reinterpret_cast<void*>(slot.addr), responseAddr, slot.len});
        EXPECT_EQ(manager_.ExecuteAsync(operation, handleOut), transport::Status::Ok);

        TransferItem item;
        item.request_index = 0;
        item.key = entry->key;
        item.remote_addr = responseAddr;
        item.len = slot.len;
        item.buffer_handle = slot.handle;

        std::vector<TransferItem> items{item};
        std::vector<std::uint32_t> results{static_cast<std::uint32_t>(ResultCode::Failed)};

        InflightRecord record;
        record.opcode = KvOpcode::Dump;
        record.handle = handleOut;
        record.transfer_items = items;
        record.request_ctx = std::make_shared<RequestContext>(KvOpcode::Dump, responseAddr,
                                                              kTargetManager, results, items);
        record.submit_ms = SteadyNowMs();
        return record;
    }

    static constexpr std::size_t kQueueCapacity = 16;
    static constexpr std::size_t kDrainBudget = 8;
    static constexpr std::size_t kHeadScanBudget = 2;

    TransHandleQueue ingress_;
    FakeMetadataIndex metadata_;
    FakeBufferManager buffers_{kValueLength};
    ProtocolManager protocols_;
    std::shared_ptr<TEST::TestTransport> testTransport_{TEST::MakeTestTransport()};
    transport::TransportManager manager_{"127.0.0.1:28000"};
};

TEST_F(CompletionPollerTest, ScansOnlyConfiguredHeadWindowAndThenPublishesAll)
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

    CompletionPoller poller;
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
    const bool allCompleted =
        WaitUntil([&]() { return testTransport_->SyncExecutionCount() == 3; });

    stop.store(true, std::memory_order_release);
    pollerThread.join();

    EXPECT_TRUE(allCompleted);
    EXPECT_TRUE(poller.Healthy());
    EXPECT_EQ(poller.PendingCount(), 0U);
    EXPECT_EQ(testTransport_->ActiveTransferCount(), 0U);
    EXPECT_EQ(metadata_.LookupReady(MakeKey(1), std::chrono::system_clock::now()),
              LookupCode::Ready);
    EXPECT_EQ(metadata_.LookupReady(MakeKey(2), std::chrono::system_clock::now()),
              LookupCode::Ready);
    EXPECT_EQ(metadata_.LookupReady(MakeKey(3), std::chrono::system_clock::now()),
              LookupCode::Ready);
}

TEST_F(CompletionPollerTest, TimeoutWaitsForTerminalThenAbortsDump)
{
    g_config.pollerDrainBudget = 1;
    g_config.pollerScanBudget = 1;
    g_config.opTimeoutMs = 1;

    TransportHandle handle = transport::kInvalidTransferHandle;
    auto record = MakeDumpRecord(7, 107, handle);
    record.submit_ms = 0;
    ASSERT_TRUE(testTransport_->SetStatus(handle, transport::TransferStatus::Waiting));
    ingress_.Push(std::move(record));

    CompletionPoller poller;
    std::atomic_bool stop{false};
    std::thread pollerThread([&]() { poller.Run(stop); });

    ASSERT_TRUE(WaitUntil([&]() { return testTransport_->QueryCount(handle) > 0; }));
    EXPECT_EQ(metadata_.LookupReady(MakeKey(7), std::chrono::system_clock::now()),
              LookupCode::NotReady);
    EXPECT_EQ(buffers_.ActiveAllocationCount(), 1U);

    ASSERT_TRUE(testTransport_->SetStatus(handle, transport::TransferStatus::Completed));
    const bool completed = WaitUntil([&]() { return testTransport_->SyncExecutionCount() == 1; });
    stop.store(true, std::memory_order_release);
    pollerThread.join();

    EXPECT_TRUE(completed);
    EXPECT_EQ(metadata_.LookupReady(MakeKey(7), std::chrono::system_clock::now()),
              LookupCode::NotFound);
    EXPECT_EQ(buffers_.ActiveAllocationCount(), 0U);
    EXPECT_EQ(testTransport_->ActiveTransferCount(), 0U);
}

}  // namespace
}  // namespace UC::DRAMPOOL
