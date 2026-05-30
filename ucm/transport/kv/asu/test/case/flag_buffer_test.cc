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
#include "flag_buffer.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace UC::ASU {
namespace {

class FlagBufferTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(FlagBufferTest, InitAndDestroy)
{
    FlagBuffer buffer;
    auto status = buffer.Init(1024 * 1024);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_NE(buffer.GetBase(), nullptr);
    ASSERT_EQ(buffer.GetCapacity(), 1024 * 1024);
    buffer.Destroy();
    ASSERT_EQ(buffer.GetBase(), nullptr);
}

TEST_F(FlagBufferTest, DoubleInit)
{
    FlagBuffer buffer;
    auto status = buffer.Init(1024);
    ASSERT_TRUE(status.ok());
    status = buffer.Init(1024);
    ASSERT_FALSE(status.ok());
    ASSERT_EQ(status.code, StatusCode::INVALID_ARGUMENT);
    buffer.Destroy();
}

TEST_F(FlagBufferTest, ZeroCapacity)
{
    FlagBuffer buffer;
    auto status = buffer.Init(0);
    ASSERT_FALSE(status.ok());
    ASSERT_EQ(status.code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(FlagBufferTest, AllocateWithoutInit)
{
    FlagBuffer buffer;
    void* data_ptr = nullptr;
    auto status = buffer.Allocate(64, data_ptr);
    ASSERT_FALSE(status.ok());
    ASSERT_EQ(status.code, StatusCode::NOT_INITIALIZED);
}

TEST_F(FlagBufferTest, AllocateZeroSize)
{
    FlagBuffer buffer;
    auto status = buffer.Init(1024);
    ASSERT_TRUE(status.ok());
    void* data_ptr = nullptr;
    status = buffer.Allocate(0, data_ptr);
    ASSERT_FALSE(status.ok());
    ASSERT_EQ(status.code, StatusCode::INVALID_ARGUMENT);
    buffer.Destroy();
}

TEST_F(FlagBufferTest, SingleAllocateAndReclaim)
{
    FlagBuffer buffer;
    auto status = buffer.Init(1024);
    ASSERT_TRUE(status.ok());

    void* data_ptr = nullptr;
    status = buffer.Allocate(64, data_ptr);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_NE(data_ptr, nullptr);

    // Verify data_ptr is within buffer bounds
    char* base = static_cast<char*>(buffer.GetBase());
    char* data = static_cast<char*>(data_ptr);
    ASSERT_GE(data, base + 8);  // At least header size offset
    ASSERT_LT(data, base + buffer.GetCapacity());

    // Write and read data
    std::memset(data_ptr, 0xAB, 64);
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(static_cast<unsigned char*>(data_ptr)[i], 0xAB);
    }

    buffer.Reclaim(data_ptr);
    buffer.Destroy();
}

TEST_F(FlagBufferTest, MultipleAllocatesAndReclaims)
{
    FlagBuffer buffer;
    auto status = buffer.Init(4096);
    ASSERT_TRUE(status.ok());

    constexpr int kCount = 10;
    std::vector<void*> ptrs(kCount);

    // Allocate all
    for (int i = 0; i < kCount; ++i) {
        status = buffer.Allocate(64, ptrs[i]);
        ASSERT_TRUE(status.ok()) << "Failed at i=" << i << ": " << status.message;
        ASSERT_NE(ptrs[i], nullptr);
        std::memset(ptrs[i], i, 64);
    }

    // Verify all data
    for (int i = 0; i < kCount; ++i) {
        for (int j = 0; j < 64; ++j) {
            ASSERT_EQ(static_cast<unsigned char*>(ptrs[i])[j], i);
        }
    }

    // Reclaim all
    for (int i = 0; i < kCount; ++i) { buffer.Reclaim(ptrs[i]); }

    buffer.Destroy();
}

TEST_F(FlagBufferTest, ReclaimNullPointer)
{
    FlagBuffer buffer;
    auto status = buffer.Init(1024);
    ASSERT_TRUE(status.ok());

    // Should not crash
    buffer.Reclaim(nullptr);

    buffer.Destroy();
}

TEST_F(FlagBufferTest, WrapAround)
{
    FlagBuffer buffer;
    auto status = buffer.Init(256);
    ASSERT_TRUE(status.ok());

    // Allocate and reclaim multiple times to force wrap-around
    for (int round = 0; round < 10; ++round) {
        void* data_ptr = nullptr;
        status = buffer.Allocate(64, data_ptr);
        ASSERT_TRUE(status.ok()) << "Failed at round=" << round << ": " << status.message;
        ASSERT_NE(data_ptr, nullptr);
        std::memset(data_ptr, round, 64);
        buffer.Reclaim(data_ptr);
    }

    buffer.Destroy();
}

TEST_F(FlagBufferTest, WrapAroundBoundary)
{
    FlagBuffer buffer;
    // Use small buffer to force wrap-around
    auto status = buffer.Init(128);
    ASSERT_TRUE(status.ok());

    // Allocate 100 bytes (8 header + 92 data), leaving 28 bytes
    void* ptr1 = nullptr;
    status = buffer.Allocate(92, ptr1);
    ASSERT_TRUE(status.ok());
    ASSERT_NE(ptr1, nullptr);

    // Reclaim ptr1 to free up space
    buffer.Reclaim(ptr1);

    // Allocate 40 bytes (8 header + 32 data), should wrap around
    // because 40 > 28 (remaining space before wrap)
    void* ptr2 = nullptr;
    status = buffer.Allocate(32, ptr2);
    ASSERT_TRUE(status.ok());
    ASSERT_NE(ptr2, nullptr);

    // ptr2 should be at the beginning of the buffer (after wrap-around)
    char* base = static_cast<char*>(buffer.GetBase());
    char* data2 = static_cast<char*>(ptr2);
    ASSERT_EQ(data2, base + 8);  // Header size offset from start

    buffer.Reclaim(ptr2);
    buffer.Destroy();
}

TEST_F(FlagBufferTest, WrapAroundWithPaddingHeader)
{
    FlagBuffer buffer;
    auto status = buffer.Init(128);
    ASSERT_TRUE(status.ok());

    // Allocate 100 bytes (8 header + 92 data), leaving 28 bytes
    void* ptr1 = nullptr;
    status = buffer.Allocate(92, ptr1);
    ASSERT_TRUE(status.ok());
    ASSERT_NE(ptr1, nullptr);

    // Reclaim ptr1
    buffer.Reclaim(ptr1);

    // Allocate 20 bytes (8 header + 12 data)
    // This fits in the 28 bytes remaining, so no wrap-around
    void* ptr2 = nullptr;
    status = buffer.Allocate(12, ptr2);
    ASSERT_TRUE(status.ok());
    ASSERT_NE(ptr2, nullptr);

    // ptr2 should be right after ptr1's space
    char* base = static_cast<char*>(buffer.GetBase());
    char* data1 = static_cast<char*>(ptr1);
    char* data2 = static_cast<char*>(ptr2);
    ASSERT_EQ(data2, data1 + 92 + 8);  // ptr1 data + ptr1 header

    // Now allocate something that will force wrap-around
    // After ptr2, we have 8 bytes left (128 - 100 - 20 = 8)
    // This is exactly sizeof(Header), so next allocation will wrap
    buffer.Reclaim(ptr2);

    void* ptr3 = nullptr;
    status = buffer.Allocate(32, ptr3);
    ASSERT_TRUE(status.ok());
    ASSERT_NE(ptr3, nullptr);

    // ptr3 should wrap around to the beginning
    char* data3 = static_cast<char*>(ptr3);
    ASSERT_EQ(data3, base + 8);

    buffer.Reclaim(ptr3);
    buffer.Destroy();
}

TEST_F(FlagBufferTest, WrapAroundWithoutPaddingHeader)
{
    FlagBuffer buffer;
    auto status = buffer.Init(128);
    ASSERT_TRUE(status.ok());

    // Allocate 104 bytes (8 header + 96 data), leaving 24 bytes
    void* ptr1 = nullptr;
    status = buffer.Allocate(96, ptr1);
    ASSERT_TRUE(status.ok());
    ASSERT_NE(ptr1, nullptr);

    // Reclaim ptr1
    buffer.Reclaim(ptr1);

    // Allocate 20 bytes (8 header + 12 data)
    // This fits in the 24 bytes remaining, but leaves only 4 bytes
    // which is < sizeof(Header), so it will be extended to consume the tail
    void* ptr2 = nullptr;
    status = buffer.Allocate(12, ptr2);
    ASSERT_TRUE(status.ok());
    ASSERT_NE(ptr2, nullptr);

    // Now try to allocate something that will force wrap-around
    void* ptr3 = nullptr;
    status = buffer.Allocate(32, ptr3);
    ASSERT_TRUE(status.ok());
    ASSERT_NE(ptr3, nullptr);

    // ptr3 should wrap around to the beginning
    char* base = static_cast<char*>(buffer.GetBase());
    char* data3 = static_cast<char*>(ptr3);
    ASSERT_EQ(data3, base + 8);

    buffer.Reclaim(ptr2);
    buffer.Reclaim(ptr3);
    buffer.Destroy();
}

TEST_F(FlagBufferTest, ExtendAllocationToConsumeTail)
{
    FlagBuffer buffer;
    auto status = buffer.Init(64);
    ASSERT_TRUE(status.ok());

    // Allocate 48 bytes (8 header + 40 data), leaving 16 bytes
    void* ptr1 = nullptr;
    status = buffer.Allocate(40, ptr1);
    ASSERT_TRUE(status.ok());
    ASSERT_NE(ptr1, nullptr);

    // Reclaim ptr1
    buffer.Reclaim(ptr1);

    // Allocate 1 byte (8 header + 1 data)
    // This leaves 7 bytes, which is less than sizeof(Header) (8 bytes)
    // According to the logic, if remaining < sizeof(Header), extend allocation
    // So the allocation will consume 8 + 1 + 7 = 16 bytes total
    void* ptr2 = nullptr;
    status = buffer.Allocate(1, ptr2);
    ASSERT_TRUE(status.ok());
    ASSERT_NE(ptr2, nullptr);

    // Verify ptr2 is at the expected location (base + 48, after ptr1's reclaimed slot)
    // The allocation consumes the remaining 7 bytes (less than sizeof(Header) = 8)
    // So the slot is at base + 48, and data is at base + 48 + 8
    char* base = static_cast<char*>(buffer.GetBase());
    char* data2 = static_cast<char*>(ptr2);
    ASSERT_EQ(data2, base + 48 + 8);  // data pointer is slot + header size

    buffer.Reclaim(ptr2);
    buffer.Destroy();
}

TEST_F(FlagBufferTest, ConcurrentAllocateAndReclaim)
{
    FlagBuffer buffer;
    auto status = buffer.Init(1024 * 1024);  // 1MB
    ASSERT_TRUE(status.ok());

    constexpr int kThreadCount = 4;
    constexpr int kOpsPerThread = 1000;

    auto worker = [&buffer](int thread_id) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            void* data_ptr = nullptr;
            auto s = buffer.Allocate(64, data_ptr);
            ASSERT_TRUE(s.ok()) << "Thread " << thread_id << " op " << i << ": " << s.message;
            ASSERT_NE(data_ptr, nullptr);

            // Write thread-specific pattern
            std::memset(data_ptr, thread_id, 64);

            // Verify data
            for (int j = 0; j < 64; ++j) {
                ASSERT_EQ(static_cast<unsigned char*>(data_ptr)[j], thread_id);
            }

            buffer.Reclaim(data_ptr);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreadCount; ++i) { threads.emplace_back(worker, i); }

    for (auto& t : threads) { t.join(); }

    buffer.Destroy();
}

TEST_F(FlagBufferTest, ConcurrentStressTest)
{
    FlagBuffer buffer;
    auto status = buffer.Init(256 * 1024);  // 256KB (larger buffer)
    ASSERT_TRUE(status.ok());

    constexpr int kThreadCount = 4;  // Fewer threads
    constexpr int kOpsPerThread = 1000;  // Fewer operations
    std::atomic<int> success_count{0};

    auto worker = [&buffer, &success_count](int thread_id) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            void* data_ptr = nullptr;
            std::size_t size = 32 + (i % 32);  // Smaller variable sizes (32-63 bytes)
            auto s = buffer.Allocate(size, data_ptr);
            if (s.ok() && data_ptr) {
                std::memset(data_ptr, thread_id, size);
                buffer.Reclaim(data_ptr);
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreadCount; ++i) { threads.emplace_back(worker, i); }

    for (auto& t : threads) { t.join(); }

    // All operations should succeed
    ASSERT_EQ(success_count.load(), kThreadCount * kOpsPerThread);

    buffer.Destroy();
}

TEST_F(FlagBufferTest, LargeAllocation)
{
    FlagBuffer buffer;
    auto status = buffer.Init(1024 * 1024);
    ASSERT_TRUE(status.ok());

    void* data_ptr = nullptr;
    status = buffer.Allocate(512 * 1024, data_ptr);  // 512KB
    ASSERT_TRUE(status.ok());
    ASSERT_NE(data_ptr, nullptr);

    std::memset(data_ptr, 0xFF, 512 * 1024);
    buffer.Reclaim(data_ptr);
    buffer.Destroy();
}

TEST_F(FlagBufferTest, ManySmallAllocations)
{
    FlagBuffer buffer;
    auto status = buffer.Init(4096);
    ASSERT_TRUE(status.ok());

    std::vector<void*> ptrs;
    constexpr int kCount = 100;

    for (int i = 0; i < kCount; ++i) {
        void* data_ptr = nullptr;
        status = buffer.Allocate(16, data_ptr);  // Small allocations
        if (status.ok()) {
            ASSERT_NE(data_ptr, nullptr);
            ptrs.push_back(data_ptr);
        }
    }

    // Reclaim all
    for (void* ptr : ptrs) { buffer.Reclaim(ptr); }

    buffer.Destroy();
}

TEST_F(FlagBufferTest, ReclaimOutOfOrder)
{
    FlagBuffer buffer;
    auto status = buffer.Init(4096);
    ASSERT_TRUE(status.ok());

    void* ptr1 = nullptr;
    void* ptr2 = nullptr;
    void* ptr3 = nullptr;

    status = buffer.Allocate(64, ptr1);
    ASSERT_TRUE(status.ok());
    status = buffer.Allocate(64, ptr2);
    ASSERT_TRUE(status.ok());
    status = buffer.Allocate(64, ptr3);
    ASSERT_TRUE(status.ok());

    // Reclaim out of order: ptr2, ptr1, ptr3
    buffer.Reclaim(ptr2);
    buffer.Reclaim(ptr1);
    buffer.Reclaim(ptr3);

    buffer.Destroy();
}

TEST_F(FlagBufferTest, DataIntegrity)
{
    FlagBuffer buffer;
    auto status = buffer.Init(4096);
    ASSERT_TRUE(status.ok());

    void* ptr1 = nullptr;
    void* ptr2 = nullptr;

    status = buffer.Allocate(128, ptr1);
    ASSERT_TRUE(status.ok());
    status = buffer.Allocate(128, ptr2);
    ASSERT_TRUE(status.ok());

    // Write different patterns
    std::memset(ptr1, 0xAA, 128);
    std::memset(ptr2, 0xBB, 128);

    // Verify no overlap
    for (int i = 0; i < 128; ++i) {
        ASSERT_EQ(static_cast<unsigned char*>(ptr1)[i], 0xAA);
        ASSERT_EQ(static_cast<unsigned char*>(ptr2)[i], 0xBB);
    }

    buffer.Reclaim(ptr1);
    buffer.Reclaim(ptr2);
    buffer.Destroy();
}

}  // namespace
}  // namespace UC::ASU
