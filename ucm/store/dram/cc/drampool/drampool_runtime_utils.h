/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace UC::DramPool {

inline constexpr auto kThreadIdleSleepDuration = std::chrono::microseconds(100);

inline std::uint64_t SteadyNowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace UC::DramPool
