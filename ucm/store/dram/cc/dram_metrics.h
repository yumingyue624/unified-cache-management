// MIT License
// Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
#pragma once

#include <chrono>
#include "metrics_api.h"
#include "types.h"

namespace UC::Dram {
// Only local task/request state carries timestamps; the KV wire format is unchanged.
using MetricClock = std::chrono::steady_clock;

struct OperationMetrics {
    explicit OperationMetrics(const std::string& prefix)
        : submitted(prefix + "tasks_submitted_total"),
          rejected(prefix + "tasks_rejected_total"),
          succeeded(prefix + "tasks_succeeded_total"),
          failed(prefix + "tasks_failed_total"),
          timeouts(prefix + "task_timeouts_total"),
          duration(prefix + "task_duration_us"),
          queueDuration(prefix + "task_queue_duration_us"),
          requests(prefix + "requests_completed_total"),
          requestFailures(prefix + "requests_failed_total"),
          requestDuration(prefix + "request_duration_us"),
          submitErrors(prefix + "request_submit_errors_total"),
          acknowledged(prefix + "acknowledged_entries_total"),
          bytes(prefix + "acknowledged_bytes_total"),
          failedEntries(prefix + "failed_entries_total"),
          unconfirmed(prefix + "unconfirmed_entries_total")
    {
    }
    Metrics::CachedMetric submitted, rejected, succeeded, failed, timeouts;
    Metrics::CachedMetric duration, queueDuration, requests, requestFailures, requestDuration,
        submitErrors;
    Metrics::CachedMetric acknowledged, bytes, failedEntries, unconfirmed;
};

inline OperationMetrics& MetricsFor(OpType op)
{
    static OperationMetrics lookup{"dramstore_lookup_"};
    static OperationMetrics dump{"dramstore_dump_"};
    static OperationMetrics load{"dramstore_load_"};
    return op == OpType::LOOKUP ? lookup : op == OpType::DUMP ? dump : load;
}

inline void RecordMetric(Metrics::CachedMetric& metric, double value = 1.0) noexcept
{
    try {
        Metrics::UpdateStats(metric, value);
    } catch (...) { /* Telemetry is best effort. */
    }
}

inline void RecordDuration(Metrics::CachedMetric& metric, MetricClock::time_point start) noexcept
{
    if (start == MetricClock::time_point{}) { return; }
    RecordMetric(metric,
                 std::chrono::duration<double, std::micro>(MetricClock::now() - start).count());
}

inline void RecordTaskResult(OpType op, MetricClock::time_point start,
                             const Status& status) noexcept
{
    try {
        auto& metrics = MetricsFor(op);
        RecordMetric(status.Success() ? metrics.succeeded : metrics.failed);
        if (status == Status::Timeout()) { RecordMetric(metrics.timeouts); }
        RecordDuration(metrics.duration, start);
    } catch (...) {
    }
}

// Metric handle initialization is also inside the exception boundary.
#define DRAM_RECORD(op, field)                                          \
    do {                                                                \
        try {                                                           \
            ::UC::Dram::RecordMetric(::UC::Dram::MetricsFor(op).field); \
        } catch (...) {                                                 \
        }                                                               \
    } while (false)
#define DRAM_EVENT(name)                                                    \
    do {                                                                    \
        try {                                                               \
            ::UC::Dram::RecordMetric(NAME_TO_METRIC_ID("dramstore_" name)); \
        } catch (...) {                                                     \
        }                                                                   \
    } while (false)

}  // namespace UC::Dram
