// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include "VkBuffer.h"

namespace sv {

class VkCtx;

struct VkRTPipeline {
    VkPipeline       pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout   = VK_NULL_HANDLE;  // not owned — caller manages lifetime
    VkBuf            sbtBuffer{};

    VkStridedDeviceAddressRegionKHR rgenRegion{};
    VkStridedDeviceAddressRegionKHR missRegion{};
    VkStridedDeviceAddressRegionKHR hitRegion{};
    VkStridedDeviceAddressRegionKHR callRegion{};  // unused, zeroed

    /// Create a ray tracing pipeline with raygen + miss + closest-hit, plus SBT.
    /// Set allowClusterAS=true to enable tracing against cluster acceleration structures.
    static VkRTPipeline create(VkCtx& ctx, VkPipelineLayout layout,
        VkShaderModule rgen, VkShaderModule rmiss, VkShaderModule rchit,
        uint32_t maxRecursionDepth = 1, bool allowClusterAS = false);

    void destroy(VkDevice device, VmaAllocator alloc);
};

} // namespace sv
