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

FlagBuffer::~FlagBuffer() { Destroy(); }

Status FlagBuffer::Init(std::size_t capacity)
{
    if (base_) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "FlagBuffer already initialized");
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

    submit_tail_.store(0, std::memory_order_relaxed);
    reclaim_head_.store(0, std::memory_order_relaxed);

    return Status::OK();
}

void FlagBuffer::Destroy()
{
    if (base_) {
        ALIGNED_FREE(base_);
        base_ = nullptr;
    }
    capacity_ = 0;
}

void* FlagBuffer::AllocateSpace(std::size_t len)
{
    while (true) {
        std::size_t tail = submit_tail_.load(std::memory_order_acquire);
        std::size_t head = reclaim_head_.load(std::memory_order_acquire);
        std::size_t used = tail - head;

        if (used + len > capacity_) { return nullptr; }

        std::size_t offset = tail % capacity_;

        // Check if allocation crosses boundary
        if (offset + len > capacity_) {
            // Place padding header to mark skipped region
            std::size_t skip_len = capacity_ - offset;
            if (skip_len >= sizeof(Header)) {
                auto* padding_header = reinterpret_cast<Header*>(
                    static_cast<char*>(base_) + offset);
                padding_header->length = static_cast<std::uint32_t>(skip_len - sizeof(Header));
                padding_header->in_use.store(false, std::memory_order_release);
            }

            std::size_t new_tail = tail + skip_len;

            if (submit_tail_.compare_exchange_weak(tail, new_tail, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
                continue;
            }
            continue;
        }

        // Check if remaining space after allocation is too small for a header
        // If so, extend current allocation to consume the tail space
        std::size_t remaining = capacity_ - (offset + len);
        if (remaining > 0 && remaining < sizeof(Header)) {
            len += remaining;
        }

        std::size_t new_tail = tail + len;

        if (submit_tail_.compare_exchange_weak(tail, new_tail, std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
            return static_cast<char*>(base_) + offset;
        }
    }
}

void FlagBuffer::TryReclaim()
{
    while (true) {
        std::size_t head = reclaim_head_.load(std::memory_order_acquire);
        std::size_t tail = submit_tail_.load(std::memory_order_acquire);

        if (head >= tail) { break; }

        std::size_t offset = head % capacity_;
        auto* header = reinterpret_cast<Header*>(static_cast<char*>(base_) + offset);

        if (header->in_use.load(std::memory_order_acquire)) { break; }

        std::size_t slot_len = sizeof(Header) + header->length;

        if (!reclaim_head_.compare_exchange_weak(head, head + slot_len, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
            continue;
        }
    }
}

Status FlagBuffer::Allocate(std::size_t size, void*& data_ptr)
{
    if (!base_) { return Status::Error(StatusCode::NOT_INITIALIZED, "FlagBuffer not initialized"); }
    if (size == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "size must be non-zero");
    }

    std::size_t total_len = sizeof(Header) + size;

    TryReclaim();

    void* slot = AllocateSpace(total_len);
    if (!slot) {
        while (true) {
            TryReclaim();
            slot = AllocateSpace(total_len);
            if (slot) { break; }
#ifdef _WIN32
            _mm_pause();
#else
            __builtin_ia32_pause();
#endif
        }
    }

    auto* header = static_cast<Header*>(slot);
    header->length = static_cast<std::uint32_t>(size);
    header->in_use.store(true, std::memory_order_release);

    data_ptr = static_cast<char*>(slot) + sizeof(Header);
    return Status::OK();
}

void FlagBuffer::Reclaim(void* data_ptr)
{
    if (!data_ptr) { return; }

    auto* header = GetHeader(data_ptr);
    if (!header) { return; }

    header->in_use.store(false, std::memory_order_release);
    TryReclaim();
}

FlagBuffer::Header* FlagBuffer::GetHeader(void* data_ptr) const
{
    if (!data_ptr || !base_) { return nullptr; }

    char* data = static_cast<char*>(data_ptr);
    char* base = static_cast<char*>(base_);

    if (data < base + sizeof(Header) || data >= base + capacity_) { return nullptr; }

    return reinterpret_cast<Header*>(data - sizeof(Header));
}

}  // namespace UC::ASU
