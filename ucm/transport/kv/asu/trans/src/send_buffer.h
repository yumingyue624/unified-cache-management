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
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "asu_transport/types.h"

namespace UC::ASU {

struct ScatterGatherEntry {
    std::uint64_t addr{0};
    std::uint32_t length{0};
    std::uint32_t lkey{0};
};

class SendBuffer {
public:
    SendBuffer() = default;
    ~SendBuffer();

    SendBuffer(const SendBuffer&) = delete;
    SendBuffer& operator=(const SendBuffer&) = delete;

    Status Init(std::size_t capacity);
    void Destroy();

    Status Allocate(std::size_t size, std::uint16_t cid, ScatterGatherEntry& sge);
    void Submit(std::uint16_t cid);
    void Cancel(std::uint16_t cid);
    void Reclaim(std::uint16_t cid);

    void* GetBase() const { return base_; }
    std::size_t GetCapacity() const { return capacity_; }

private:
    static constexpr std::size_t kAlignment = 512;
    static constexpr std::size_t kMaxCIDCount = 65536;  // 16-bit CID range
    static constexpr std::size_t kMaxROBEntries = 65536;
    static constexpr std::size_t kInvalidROBIndex = SIZE_MAX;

    struct ReorderEntry {
        std::size_t length{0};
        std::atomic<bool> submitted{false};
        std::atomic<bool> completed{false};
    };

    void* base_{nullptr};
    std::size_t capacity_{0};

    std::atomic<std::size_t> submit_tail_{0};
    std::atomic<std::size_t> reclaim_head_{0};

    std::unique_ptr<ReorderEntry[]> rob_;
    std::atomic<std::size_t> rob_head_{0};
    std::atomic<std::size_t> rob_tail_{0};

    std::vector<std::size_t> cid_to_rob_;

    void* AllocateSpace(std::size_t len);
    void TryReclaim();
};

}  // namespace UC::ASU
