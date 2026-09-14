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

#include "kv_protocol.h"
#include "metrics_api.h"

// Keep each operation's metric name a compile-time string literal so
// NAME_TO_METRIC_ID retains its call-site-local CachedMetric.
#define DRAMSTORE_OP_METRIC(op, suffix)                                                   \
    ((op) == ::UC::DramPool::OpType::LOOKUP                                               \
         ? NAME_TO_METRIC_ID("dramstore_lookup_" suffix)                                  \
         : (op) == ::UC::DramPool::OpType::DUMP                                           \
               ? NAME_TO_METRIC_ID("dramstore_dump_" suffix)                              \
               : NAME_TO_METRIC_ID("dramstore_load_" suffix))
