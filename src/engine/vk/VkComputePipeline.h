// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>

namespace sv {

// Minimal compute pipeline wrapper
struct VkComputePipe {
    VkPipeline       pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout   = VK_NULL_HANDLE;  // not owned, caller manages lifetime

    static VkComputePipe create(VkDevice device, VkShaderModule comp, VkPipelineLayout layout);
    void destroy(VkDevice device);
};

} // namespace sv
