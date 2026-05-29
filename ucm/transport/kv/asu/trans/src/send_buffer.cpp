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
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <malloc.h>
#define ALIGNED_ALLOC(size, alignment) _aligned_malloc(size, alignment)
#define ALIGNED_FREE(ptr) _aligned_free(ptr)
#else
#include <immintrin.h>
#define ALIGNED_ALLOC(size, alignment)                          \
    ({                                                          \
        void* p;                                                \
        posix_memalign(&p, alignment, size) == 0 ? p : nullptr; \
    })
#define ALIGNED_FREE(ptr) free(ptr)
#endif

namespace UC::ASU {

SendBuffer::~SendBuffer() { Destroy(); }

Status SendBuffer::Init(std::size_t capacity)
{
    if (base_) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "SendBuffer already initialized");
    }
    if (capacity == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "capacity must be non-zero");
    }

    void* mem = ALIGNED_ALLOC(capacity, kAlignment);
    if (!mem) {
        return Status::Error(StatusCode::INTERNAL_ERROR, "failed to allocate aligned memory");
    }
    std::memset(mem, 0, capacity);

    base_ = mem;
    capacity_ = capacity;

    rob_ = std::make_unique<ReorderEntry[]>(kMaxROBEntries);
    cid_to_rob_.assign(kMaxCIDCount, kInvalidROBIndex);

    submit_tail_.store(0, std::memory_order_relaxed);
    reclaim_head_.store(0, std::memory_order_relaxed);
    rob_head_.store(0, std::memory_order_relaxed);
    rob_tail_.store(0, std::memory_order_relaxed);

    return Status::OK();
}

void SendBuffer::Destroy()
{
    if (base_) {
        ALIGNED_FREE(base_);
        base_ = nullptr;
    }
    capacity_ = 0;
    rob_.reset();
    cid_to_rob_.clear();
}

void* SendBuffer::AllocateSpace(std::size_t len)
{
    while (true) {
        std::size_t tail = submit_tail_.load(std::memory_order_relaxed);
        std::size_t head = reclaim_head_.load(std::memory_order_acquire);

        std::size_t used = tail - head;
        std::size_t offset = tail % capacity_;

        std::size_t actual_len = len;
        void* addr = nullptr;

        if (offset + len > capacity_) {
            // Wrap-around: skip tail padding, allocate from head
            actual_len = len + (capacity_ - offset);
            addr = base_;
        } else {
            addr = static_cast<char*>(base_) + offset;
        }

        if (used + actual_len > capacity_) { return nullptr; }

        if (submit_tail_.compare_exchange_weak(tail, tail + actual_len, std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
            return addr;
        }
    }
}

Status SendBuffer::Allocate(std::size_t size, std::uint16_t cid, ScatterGatherEntry& sge)
{
    if (!base_) { return Status::Error(StatusCode::NOT_INITIALIZED, "SendBuffer not initialized"); }
    if (size == 0 || size % 4 != 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "size must be non-zero and 4-byte aligned");
    }
    if (size > capacity_) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "size exceeds buffer capacity");
    }

    TryReclaim();

    void* addr = AllocateSpace(size);
    if (!addr) {
        while (true) {
            TryReclaim();
            addr = AllocateSpace(size);
            if (addr) { break; }
#ifdef _WIN32
            _mm_pause();
#else
            __builtin_ia32_pause();
#endif
        }
    }

    std::size_t rob_idx = rob_tail_.fetch_add(1, std::memory_order_acq_rel) % kMaxROBEntries;
    rob_[rob_idx].length = size;
    rob_[rob_idx].submitted.store(false, std::memory_order_relaxed);
    rob_[rob_idx].completed.store(false, std::memory_order_relaxed);

    cid_to_rob_[cid] = rob_idx;

    sge.addr = reinterpret_cast<std::uint64_t>(addr);
    sge.length = static_cast<std::uint32_t>(size);
    sge.lkey = 0;

    return Status::OK();
}

void SendBuffer::Submit(std::uint16_t cid)
{
    std::size_t rob_idx = cid_to_rob_[cid];
    if (rob_idx == kInvalidROBIndex) { return; }

    rob_[rob_idx].submitted.store(true, std::memory_order_release);
    TryReclaim();
}

void SendBuffer::Cancel(std::uint16_t cid)
{
    std::size_t rob_idx = cid_to_rob_[cid];
    if (rob_idx == kInvalidROBIndex) { return; }

    rob_[rob_idx].submitted.store(true, std::memory_order_relaxed);
    rob_[rob_idx].completed.store(true, std::memory_order_release);
    TryReclaim();
}

void SendBuffer::Reclaim(std::uint16_t cid)
{
    std::size_t rob_idx = cid_to_rob_[cid];
    if (rob_idx == kInvalidROBIndex) { return; }

    rob_[rob_idx].completed.store(true, std::memory_order_release);
    TryReclaim();
}

void SendBuffer::TryReclaim()
{
    while (true) {
        std::size_t head = rob_head_.load(std::memory_order_acquire);
        std::size_t idx = head % kMaxROBEntries;

        if (!rob_[idx].submitted.load(std::memory_order_acquire) ||
            !rob_[idx].completed.load(std::memory_order_acquire)) {
            break;
        }

        if (!rob_head_.compare_exchange_weak(head, head + 1, std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
            continue;
        }

        reclaim_head_.fetch_add(rob_[idx].length, std::memory_order_release);
    }
}

}  // namespace UC::ASU
