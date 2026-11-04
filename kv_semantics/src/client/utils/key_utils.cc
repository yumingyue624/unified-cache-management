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
#include "key_utils.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <string>
#include "kv_client.h"
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace kv {

Status ValidateKeyResolutionConfig(const KeyResolutionConfig& config)
{
    if (config.engine == InferenceEngine::GENERIC || config.engine == InferenceEngine::SGLANG) {
        return Status::OK();
    }
    if (config.engine != InferenceEngine::VLLM_ASCEND) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "unsupported inference engine");
    }
    if (config.layerCount == 0 || config.layerSlotBytes == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "vllm_ascend requires positive layer_count and layer_slot_bytes");
    }
    // KVBuffer carries a uint32 object offset.
    constexpr auto maxObjectBytes = std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1;
    if (config.layerCount > maxObjectBytes / config.layerSlotBytes) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "layer layout exceeds uint32 offsets");
    }
    return Status::OK();
}

Status KvClient::ResolveKey(std::string_view key, std::string_view namespace_id,
                            ResolvedKey& result) const
{
    return ResolveObjectKey(KeyResolutionConfig{}, key, namespace_id, result);
}

Status ResolveObjectKey(const KeyResolutionConfig& config, std::string_view key,
                        std::string_view namespace_id, ResolvedKey& result)
{
    auto identity = key;
    std::uint64_t baseOffset = 0;
    if (config.engine == InferenceEngine::VLLM_ASCEND) {
        // LayerPoolKey ends with @chunk_hash@layer. Preserve every other field.
        const auto layerSeparator = key.rfind('@');
        const auto hashSeparator = layerSeparator != std::string_view::npos && layerSeparator != 0
                                       ? key.rfind('@', layerSeparator - 1)
                                       : std::string_view::npos;
        if (hashSeparator != std::string_view::npos && hashSeparator != 0) {
            const auto hash = key.substr(hashSeparator + 1, layerSeparator - hashSeparator - 1);
            const auto layerText = key.substr(layerSeparator + 1);
            const bool numeric =
                !layerText.empty() && std::all_of(layerText.begin(), layerText.end(),
                                                  [](char c) { return c >= '0' && c <= '9'; });
            if (!hash.empty() && hash.find(':') == std::string_view::npos && !layerText.empty() &&
                (numeric || layerText.front() == '-' || layerText.front() == '+')) {
                std::uint64_t layer = 0;
                const auto [end, error] =
                    std::from_chars(layerText.data(), layerText.data() + layerText.size(), layer);
                if (error != std::errc{} || end != layerText.data() + layerText.size() ||
                    layer >= config.layerCount) {
                    return Status::Error(StatusCode::INVALID_ARGUMENT,
                                         "invalid layer in key: " + std::string(key));
                }
                identity = key.substr(0, layerSeparator);
                baseOffset = layer * config.layerSlotBytes;
            }
        }
    }
    // Match Mooncake TenantId::MakeScopedKey, including the NUL separator.
    std::string scoped(namespace_id);
    scoped.push_back('\0');
    scoped.append(identity.data(), identity.size());
    const auto hash = XXH64(scoped.data(), scoped.size(), 0);
    XXH64_canonical_t canonical;
    XXH64_canonicalFromHash(&canonical, hash);
    static_assert(kCacheKeySizeBytes == sizeof(canonical.digest));
    ResolvedKey resolved;
    std::memcpy(resolved.key.data(), canonical.digest, resolved.key.size());
    resolved.base_offset = baseOffset;
    result = resolved;
    return Status::OK();
}

}  // namespace kv
