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

namespace UC::DRAMPOOL {
namespace {

constexpr auto kConditionWaitTimeout = std::chrono::seconds(2);
constexpr auto kConditionPollInterval = std::chrono::milliseconds(1);
constexpr std::uint64_t kTestOperationTimeoutMs = 60'000;
constexpr std::uint32_t kTestIdleWaitUs = 50;

std::uint64_t NowMs()
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

InflightRecord MakeDumpRecord(std::uint8_t keySuffix, std::uint64_t responseAddr,
                              FakeMetadataIndex& metadata, FakeBufferManager& buffers,
                              FakeTransportManager& transport, TransportHandle& handleOut)
{
    constexpr std::uint32_t kValueLength = 16;
    auto allocated = buffers.Allocate(kValueLength);
    EXPECT_TRUE(allocated.HasValue());
    auto slot = std::move(allocated).Value();

    EntryCreateOptions entry;
    entry.key = MakeKey(keySuffix);
    entry.buffer_handle = slot.handle;
    entry.local_addr = slot.addr;
    entry.len = slot.len;
    const auto reserved = metadata.ReserveDumpEntry(entry);
    EXPECT_EQ(reserved.code, ReserveDumpCode::Reserved);

    TransportOp operation;
    operation.opcode = KvOpcode::Dump;
    operation.direction = TransportDirection::ReadRemoteToLocal;
    operation.segments.push_back(TransportSegment{slot.addr, responseAddr, slot.len});
    auto submitted = transport.SubmitAsync(operation);
    EXPECT_TRUE(submitted.HasValue());
    handleOut = std::move(submitted).Value();

    TransferItem item;
    item.request_index = 0;
    item.key = entry.key;
    item.remote_addr = responseAddr;
    item.len = slot.len;
    item.buffer_handle = slot.handle;

    std::vector<TransferItem> items{item};
    std::vector<std::uint32_t> results{ToResultValue(ResultCode::Failed)};

    InflightRecord record;
    record.opcode = KvOpcode::Dump;
    record.handle = handleOut;
    record.transfer_items = items;
    record.request_ctx =
        std::make_shared<RequestContext>(KvOpcode::Dump, responseAddr, results, items);
    record.submit_ms = NowMs();
    return record;
}

TEST(CompletionPollerTest, ScansOnlyConfiguredHeadWindowAndThenPublishesAll)
{
    constexpr std::size_t kQueueCapacity = 16;
    constexpr std::size_t kDrainBudget = 8;
    constexpr std::size_t kHeadScanBudget = 2;
    constexpr std::size_t kMaxPending = 16;

    TransHandleQueue ingress;
    ingress.Setup(kQueueCapacity);
    FakeMetadataIndex metadata;
    FakeBufferManager buffers;
    FakeTransportManager transport;
    FakeResponseWriter responses;

    TransportHandle firstHandle;
    TransportHandle secondHandle;
    TransportHandle thirdHandle;
    auto first = MakeDumpRecord(1, 101, metadata, buffers, transport, firstHandle);
    auto second = MakeDumpRecord(2, 102, metadata, buffers, transport, secondHandle);
    auto third = MakeDumpRecord(3, 103, metadata, buffers, transport, thirdHandle);

    ASSERT_TRUE(transport.SetStatus(firstHandle, TransportStatus::Waiting).Success());
    ASSERT_TRUE(transport.SetStatus(secondHandle, TransportStatus::Waiting).Success());
    ingress.Push(std::move(first));
    ingress.Push(std::move(second));
    ingress.Push(std::move(third));

    CompletionPollerOptions options;
    options.drain_budget = kDrainBudget;
    options.scan_budget = kHeadScanBudget;
    options.max_pending = kMaxPending;
    options.operation_timeout_ms = kTestOperationTimeoutMs;
    options.idle_wait_us = kTestIdleWaitUs;
    CompletionPoller poller(ingress, metadata, buffers, transport, responses, options);

    std::atomic_bool stop{false};
    std::thread pollerThread([&]() { poller.Run(stop); });

    const bool headWasScanned = WaitUntil([&]() {
        return transport.QueryCount(firstHandle) > 0 && transport.QueryCount(secondHandle) > 0;
    });
    EXPECT_TRUE(headWasScanned);
    if (!headWasScanned) {
        stop.store(true, std::memory_order_release);
        pollerThread.join();
        return;
    }

    EXPECT_EQ(transport.QueryCount(thirdHandle), 0U);
    EXPECT_EQ(responses.ResponseCount(), 0U);

    EXPECT_TRUE(transport.SetStatus(firstHandle, TransportStatus::Success).Success());
    EXPECT_TRUE(transport.SetStatus(secondHandle, TransportStatus::Success).Success());
    const bool allCompleted = WaitUntil([&]() { return responses.ResponseCount() == 3; });

    stop.store(true, std::memory_order_release);
    pollerThread.join();

    EXPECT_TRUE(allCompleted);
    EXPECT_TRUE(poller.Healthy());
    EXPECT_EQ(poller.PendingCount(), 0U);
    EXPECT_EQ(transport.ActiveHandleCount(), 0U);
    EXPECT_EQ(metadata.LookupReady(MakeKey(1), NowMs()), LookupCode::Ready);
    EXPECT_EQ(metadata.LookupReady(MakeKey(2), NowMs()), LookupCode::Ready);
    EXPECT_EQ(metadata.LookupReady(MakeKey(3), NowMs()), LookupCode::Ready);
}

TEST(CompletionPollerTest, TimeoutCancelsDumpThenAbortsMetadataAndFreesBuffer)
{
    constexpr std::size_t kQueueCapacity = 4;
    constexpr std::size_t kPollBudget = 1;
    constexpr std::size_t kMaxPending = 4;
    constexpr std::uint64_t kImmediateTimeoutMs = 1;

    TransHandleQueue ingress;
    ingress.Setup(kQueueCapacity);
    FakeMetadataIndex metadata;
    FakeBufferManager buffers;
    FakeTransportManager transport;
    FakeResponseWriter responses;

    TransportHandle handle;
    auto record = MakeDumpRecord(7, 107, metadata, buffers, transport, handle);
    record.submit_ms = 0;
    ASSERT_TRUE(transport.SetStatus(handle, TransportStatus::Waiting).Success());
    ingress.Push(std::move(record));

    CompletionPollerOptions options;
    options.drain_budget = kPollBudget;
    options.scan_budget = kPollBudget;
    options.max_pending = kMaxPending;
    options.operation_timeout_ms = kImmediateTimeoutMs;
    options.idle_wait_us = kTestIdleWaitUs;
    CompletionPoller poller(ingress, metadata, buffers, transport, responses, options);

    std::atomic_bool stop{false};
    std::thread pollerThread([&]() { poller.Run(stop); });
    const bool completed = WaitUntil([&]() { return responses.ResponseCount() == 1; });
    stop.store(true, std::memory_order_release);
    pollerThread.join();

    EXPECT_TRUE(completed);
    EXPECT_EQ(metadata.LookupReady(MakeKey(7), NowMs()), LookupCode::NotFound);
    EXPECT_EQ(buffers.ActiveAllocationCount(), 0U);
    EXPECT_EQ(transport.ActiveHandleCount(), 0U);
    ASSERT_EQ(responses.LastResults().size(), 1U);
    EXPECT_EQ(responses.LastResults().front(), ToResultValue(ResultCode::Failed));
}

}  // namespace
}  // namespace UC::DRAMPOOL
