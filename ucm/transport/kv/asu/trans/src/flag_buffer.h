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
#include "asu_transport/types.h"

namespace UC::ASU {

class FlagBuffer {
public:
    FlagBuffer() = default;
    ~FlagBuffer();

    FlagBuffer(const FlagBuffer&) = delete;
    FlagBuffer& operator=(const FlagBuffer&) = delete;

    Status Init(std::size_t capacity);
    void Destroy();

    Status Allocate(std::size_t size, void*& data_ptr);
    void Reclaim(void* data_ptr);

    void* GetBase() const { return base_; }
    std::size_t GetCapacity() const { return capacity_; }

private:
    static constexpr std::size_t kAlignment = 512;

    struct Header {
        std::uint32_t length;
        std::atomic<bool> in_use;
        std::uint8_t padding[3];
    };

    static_assert(sizeof(Header) == 8, "Header must be 8 bytes");

    void* base_{nullptr};
    std::size_t capacity_{0};

    std::atomic<std::size_t> submit_tail_{0};
    std::atomic<std::size_t> reclaim_head_{0};

    void* AllocateSpace(std::size_t len);
    void TryReclaim();
    Header* GetHeader(void* data_ptr) const;
};

}  // namespace UC::ASU
