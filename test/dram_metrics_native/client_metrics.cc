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
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include "metrics_api.h"
#include "node_actor.h"
#include "router/router.h"
#include "task_manager.h"

using namespace UC;
using namespace UC::Dram;
using namespace std::chrono_literals;

void Register()
{
    Metrics::SetUp();
    for (const auto* prefix : {"dramstore_dump_", "dramstore_lookup_"}) {
        for (const auto* suffix :
             {"tasks_submitted_total", "tasks_rejected_total", "tasks_succeeded_total",
              "tasks_failed_total", "requests_completed_total", "requests_failed_total",
              "acknowledged_entries_total", "acknowledged_bytes_total", "failed_entries_total",
              "unconfirmed_entries_total", "task_timeouts_total", "request_submit_errors_total"}) {
            Metrics::CreateStats(std::string(prefix) + suffix, "counter");
        }
        for (const auto* suffix :
             {"task_duration_us", "task_queue_duration_us", "request_duration_us"}) {
            Metrics::CreateStats(std::string(prefix) + suffix, "histogram", {100, 1000, 1000000});
        }
    }
    Metrics::CreateStats("dramstore_stale_replies_total", "counter");
}

void TestNodeResults()
{
    std::array<uint8_t, 64> reply{};
    std::vector<RequestCompleted> completed;
    RequestToken token;
    NodeDependencies deps{
        [&](std::vector<RequestCompleted>& events) {
            for (auto& e : events) { completed.push_back(std::move(e)); }
            events.clear();
        },
        [&](TransportCommand& command) {
            if (auto* transmit = std::get_if<Transmit>(&command)) { token = transmit->token; }
            return Status::OK();
        },
        [&](const RequestToken&, OpType, size_t) -> Expected<ReplySlot> {
            return ReplySlot{reply.data(), reply.data(), reply.size(), 0};
        },
        [](const RequestToken&, const ReplySlot&) { return Status::OK(); },
    };
    NodeActor actor(
        {
            {1, "127.0.0.1", 12345, "127.0.0.1:23456"},
            {4, 8},
            1ms
    },
        std::move(deps));
    const auto now = std::chrono::steady_clock::now();
    actor.Advance(now);
    actor.Handle(
        NodeEvent{
            ConnectCompleted{1, 0, 1, Status::OK()}
    },
        now);
    Request request;
    request.taskId = 1;
    request.requestId = 1;
    request.nodeId = 1;
    request.op = OpType::DUMP;
    request.deadline = now + 1h;
    for (uint8_t i = 0; i < 2; ++i) {
        IoEntry entry;
        entry.blockId[0] = static_cast<std::byte>(i + 1);
        entry.buffer = {0x1000, 64};
        request.entries.push_back(entry);
    }
    actor.Handle(std::move(request), now);
    actor.Advance(now);
    actor.Handle(
        NodeEvent{
            TransmitCompleted{token, Status::OK()}
    },
        now);
    actor.Handle(
        NodeEvent{
            ReplyObserved{token, Status::OK(), {{0, true, 0}, {1, false, 1}}}
    },
        now);
    actor.Advance(now);
    actor.Handle(
        NodeEvent{
            ReplyObserved{token, Status::OK(), {{0, true, 0}, {1, false, 1}}}
    },
        now);
    actor.Advance(now);
    assert(completed.size() == 1 && completed[0].status.Failure());
    auto [c, g, h] = Metrics::GetAllStatsAndClear();
    assert(c["dramstore_dump_requests_completed_total"] == 1);
    assert(c["dramstore_dump_requests_failed_total"] == 1);
    assert(c["dramstore_dump_acknowledged_entries_total"] == 1);
    assert(c["dramstore_dump_acknowledged_bytes_total"] == 64);
    assert(c["dramstore_dump_failed_entries_total"] == 1);
    assert(c["dramstore_stale_replies_total"] == 1);
}

class SingleRouter final : public Router::Router {
public:
    SingleRouter() : Router([](const std::string&) { return 0ULL; }) {}

private:
    UC::Router::NodeId RouteKey(const UC::Router::CacheKey&) const override { return 1; }
};

void TestTaskCounting()
{
    TaskManager* managerPtr = nullptr;
    TaskManagerConfig config{
        {64},
        16, 8, {1s, 1s, 1s}
    };
    TaskManager manager(
        config, {std::make_shared<SingleRouter>(), [&](Request& request) {
                     std::vector<RequestCompleted> events{
                         {request.taskId, request.requestId, 1, Status::OK(), {{0, true, 0}}}
                     };
                     managerPtr->Publish(events);
                     return Status::OK();
                 }});
    managerPtr = &manager;
    assert(manager.Start().Success());
    Detail::BlockId key{};
    key[0] = std::byte{1};
    auto submitted = manager.SubmitLookup(&key, 1);
    assert(submitted);
    const auto id = submitted.Value();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (true) {
        auto checked = manager.Check(id);
        assert(checked);
        if (checked.Value()) { break; }
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    for (int i = 0; i < 10; ++i) { assert(manager.Check(id).Value()); }
    assert(manager.WaitLookup(id));
    manager.Shutdown();
    assert(!manager.SubmitLookup(&key, 1));
    auto [c, g, h] = Metrics::GetAllStatsAndClear();
    assert(c["dramstore_lookup_tasks_submitted_total"] == 1);
    assert(c["dramstore_lookup_tasks_succeeded_total"] == 1);
    assert(c["dramstore_lookup_tasks_rejected_total"] == 1);
    uint64_t count = 0;
    for (auto bucket : h.at("dramstore_lookup_task_duration_us").bucketCounts) { count += bucket; }
    assert(count == 1);
}

int main()
{
    Register();
    TestNodeResults();
    TestTaskCounting();
    std::cout << "client metrics state-machine tests passed\n";
}
