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

#include <memory>
#include <unordered_map>
#include "sqe.h"
#include "send_buffer.h"

namespace UC::ASU {

class SqeManager {
public:
    SqeManager() = default;
    ~SqeManager() = default;

    SqeManager(const SqeManager&) = delete;
    SqeManager& operator=(const SqeManager&) = delete;

    // 初始化：创建 7 种 Sqe 对象
    Status Init(SendBuffer& send_buffer);

    // 发送请求：Allocate + Pack + Submit/Cancel
    // 成功时返回 Status::OK() 并填充 sge
    // 失败时返回错误码，内部已调用 Cancel
    Status SendRequest(SqeOpcode opcode, const SqeRequest& req, ScatterGatherEntry& sge);

private:
    SendBuffer* send_buffer_{nullptr};
    std::unordered_map<SqeOpcode, std::unique_ptr<Sqe>> packers_;

    const Sqe* GetSqe(SqeOpcode opcode) const;
};

}  // namespace UC::ASU
