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
#include "sqe_manager.h"

namespace UC::ASU {

Status SqeManager::Init(SendBuffer& send_buffer)
{
    send_buffer_ = &send_buffer;

    // 创建 7 种 SQE 打包器
    packers_[SqeOpcode::Store] = std::make_unique<KvStoreSqe>();
    packers_[SqeOpcode::Retrieve] = std::make_unique<KvRetrieveSqe>();
    packers_[SqeOpcode::BatchStore] = std::make_unique<KvBatchStoreSqe>();
    packers_[SqeOpcode::BatchRetrieve] = std::make_unique<KvBatchRetrieveSqe>();
    packers_[SqeOpcode::Delete] = std::make_unique<KvDeleteSqe>();
    packers_[SqeOpcode::Exist] = std::make_unique<KvExistSqe>();
    packers_[SqeOpcode::KeepAlive] = std::make_unique<KvKeepAliveSqe>();

    return Status::OK();
}

Status SqeManager::SendRequest(SqeOpcode opcode, const SqeRequest& req, ScatterGatherEntry& sge)
{
    // 1. 查找对应的 Sqe 打包器
    const Sqe* sqe = GetSqe(opcode);
    if (!sqe) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "unknown SQE opcode");
    }

    // 2. 分配空间
    std::size_t size = sqe->PackedSize(req);
    auto status = send_buffer_->Allocate(size, req.cid, sge);
    if (!status.ok()) {
        return status;  // 分配失败，直接返回（不需要 Cancel）
    }

    // 3. 打包
    status = sqe->Pack(req, reinterpret_cast<std::uint32_t*>(sge.addr));
    if (!status.ok()) {
        send_buffer_->Cancel(req.cid);  // 打包失败，Cancel
        return status;
    }

    return Status::OK();
}

const Sqe* SqeManager::GetSqe(SqeOpcode opcode) const
{
    auto it = packers_.find(opcode);
    if (it == packers_.end()) {
        return nullptr;
    }
    return it->second.get();
}

}  // namespace UC::ASU
