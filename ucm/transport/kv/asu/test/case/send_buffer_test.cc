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
#include "send_buffer.h"
#include <cstring>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace UC::ASU {
namespace {

class SendBufferTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(SendBufferTest, InitAndDestroy)
{
    SendBuffer buffer;
    auto status = buffer.Init(1024 * 1024);  // 1MB
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_NE(buffer.GetBase(), nullptr);
    ASSERT_EQ(buffer.GetCapacity(), 1024 * 1024);
    buffer.Destroy();
    ASSERT_EQ(buffer.GetBase(), nullptr);
}

TEST_F(SendBufferTest, InitWithZeroCapacity)
{
    SendBuffer buffer;
    auto status = buffer.Init(0);
    ASSERT_FALSE(status.ok());
    ASSERT_EQ(status.code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(SendBufferTest, DoubleInit)
{
    SendBuffer buffer;
    auto status = buffer.Init(1024 * 1024);
    ASSERT_TRUE(status.ok());
    status = buffer.Init(1024 * 1024);
    ASSERT_FALSE(status.ok());
    ASSERT_EQ(status.code, StatusCode::INVALID_ARGUMENT);
    buffer.Destroy();
}

TEST_F(SendBufferTest, AllocateWithoutInit)
{
    SendBuffer buffer;
    ScatterGatherEntry sge;
    auto status = buffer.Allocate(64, 1, sge);
    ASSERT_FALSE(status.ok());
    ASSERT_EQ(status.code, StatusCode::NOT_INITIALIZED);
}

TEST_F(SendBufferTest, SingleAllocateAndReclaim)
{
    SendBuffer buffer;
    auto status = buffer.Init(1024 * 1024);
    ASSERT_TRUE(status.ok());

    ScatterGatherEntry sge;
    status = buffer.Allocate(64, 1, sge);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_NE(sge.addr, 0);
    ASSERT_EQ(sge.length, 64);
    ASSERT_EQ(sge.lkey, 0);

    // Verify address is within buffer
    auto* base = static_cast<char*>(buffer.GetBase());
    auto* addr = reinterpret_cast<char*>(sge.addr);
    ASSERT_GE(addr, base);
    ASSERT_LT(addr, base + buffer.GetCapacity());

    // Write some data
    std::memset(addr, 0xAB, 64);

    // Reclaim
    buffer.Reclaim(1);

    buffer.Destroy();
}

TEST_F(SendBufferTest, MultipleAllocates)
{
    SendBuffer buffer;
    auto status = buffer.Init(1024 * 1024);
    ASSERT_TRUE(status.ok());

    constexpr int kCount = 100;
    std::vector<ScatterGatherEntry> sges(kCount);

    for (int i = 0; i < kCount; ++i) {
        status = buffer.Allocate(64, static_cast<std::uint16_t>(i + 1), sges[i]);
        ASSERT_TRUE(status.ok()) << "Failed at i=" << i << ": " << status.message;
    }

    for (int i = 0; i < kCount; ++i) { buffer.Reclaim(static_cast<std::uint16_t>(i + 1)); }

    buffer.Destroy();
}

TEST_F(SendBufferTest, CancelAllocation)
{
    SendBuffer buffer;
    auto status = buffer.Init(1024 * 1024);
    ASSERT_TRUE(status.ok());

    ScatterGatherEntry sge;
    status = buffer.Allocate(64, 1, sge);
    ASSERT_TRUE(status.ok());

    // Cancel instead of submit
    buffer.Cancel(1);

    // Should be able to allocate again with same CID
    status = buffer.Allocate(64, 1, sge);
    ASSERT_TRUE(status.ok());
    buffer.Reclaim(1);

    buffer.Destroy();
}

TEST_F(SendBufferTest, InvalidSize)
{
    SendBuffer buffer;
    auto status = buffer.Init(1024 * 1024);
    ASSERT_TRUE(status.ok());

    ScatterGatherEntry sge;

    // Zero size
    status = buffer.Allocate(0, 1, sge);
    ASSERT_FALSE(status.ok());
    ASSERT_EQ(status.code, StatusCode::INVALID_ARGUMENT);

    // Not 4-byte aligned
    status = buffer.Allocate(65, 1, sge);
    ASSERT_FALSE(status.ok());
    ASSERT_EQ(status.code, StatusCode::INVALID_ARGUMENT);

    // Exceeds capacity
    status = buffer.Allocate(2 * 1024 * 1024, 1, sge);
    ASSERT_FALSE(status.ok());
    ASSERT_EQ(status.code, StatusCode::INVALID_ARGUMENT);

    buffer.Destroy();
}

TEST_F(SendBufferTest, WrapAround)
{
    SendBuffer buffer;
    auto status = buffer.Init(1024);  // Small buffer
    ASSERT_TRUE(status.ok());

    // Allocate and reclaim multiple times to force wrap-around
    for (int round = 0; round < 10; ++round) {
        ScatterGatherEntry sge;
        status = buffer.Allocate(256, static_cast<std::uint16_t>(round + 1), sge);
        ASSERT_TRUE(status.ok()) << "Failed at round=" << round << ": " << status.message;
        buffer.Reclaim(static_cast<std::uint16_t>(round + 1));
    }

    buffer.Destroy();
}

TEST_F(SendBufferTest, WrapAroundAddressCorrectness)
{
    SendBuffer buffer;
    auto status = buffer.Init(1024);  // Small buffer
    ASSERT_TRUE(status.ok());

    void* base = buffer.GetBase();

    // Step 1: Allocate 900 bytes to push submit_tail near the end
    ScatterGatherEntry sge1;
    status = buffer.Allocate(900, 1, sge1);
    ASSERT_TRUE(status.ok());
    ASSERT_EQ(reinterpret_cast<void*>(sge1.addr), base);  // Should start at base

    // Step 2: Reclaim to free up space at the head
    buffer.Reclaim(1);

    // Step 3: Allocate 200 bytes - this should wrap around
    // submit_tail is at 900, capacity is 1024, so offset = 900
    // We want 200 bytes, but only 124 bytes left at the tail (1024 - 900)
    // This should wrap around and allocate at the head (base_)
    ScatterGatherEntry sge2;
    status = buffer.Allocate(200, 2, sge2);
    ASSERT_TRUE(status.ok()) << "Wrap-around allocation failed: " << status.message;

    // The key test: address should be at base_, not at base_ + 900
    ASSERT_EQ(reinterpret_cast<void*>(sge2.addr), base)
        << "Wrap-around should allocate at head, not at tail offset";

    // Verify we can write to the entire 200 bytes without going out of bounds
    std::memset(reinterpret_cast<void*>(sge2.addr), 0xAB, 200);

    // Verify the data was written correctly
    auto* data = reinterpret_cast<unsigned char*>(sge2.addr);
    for (int i = 0; i < 200; ++i) {
        ASSERT_EQ(data[i], 0xAB) << "Data corruption at byte " << i;
    }

    buffer.Reclaim(2);

    buffer.Destroy();
}

TEST_F(SendBufferTest, ConcurrentAllocateAndReclaim)
{
    SendBuffer buffer;
    auto status = buffer.Init(1024 * 1024);  // 1MB
    ASSERT_TRUE(status.ok());

    constexpr int kThreadCount = 4;
    constexpr int kOpsPerThread = 1000;

    auto worker = [&buffer](int thread_id) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            std::uint16_t cid = static_cast<std::uint16_t>(thread_id * kOpsPerThread + i + 1);
            ScatterGatherEntry sge;
            auto s = buffer.Allocate(64, cid, sge);
            ASSERT_TRUE(s.ok()) << "Thread " << thread_id << " op " << i << ": " << s.message;

            // Write some data
            std::memset(reinterpret_cast<void*>(sge.addr), thread_id, 64);

            buffer.Reclaim(cid);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreadCount; ++i) { threads.emplace_back(worker, i); }

    for (auto& t : threads) { t.join(); }

    buffer.Destroy();
}

}  // namespace
}  // namespace UC::ASU
