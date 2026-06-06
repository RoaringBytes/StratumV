// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "Types.h"
#include <volk.h>
#include <cstdint>

namespace sv {

class VkCtx;
class Config;

// Per-frame data passed to each render pass
struct FrameData {
    VkCommandBuffer cmd;
    uint32_t        frameIndex;      // global monotonic counter
    uint32_t        currentFrame;    // double-buffer index (0 or 1)
    uint32_t        imageIndex;      // swapchain image index
    const SceneUBO* sceneUBO;
    VkDescriptorSet sceneDescSet;
    uint32_t        renderWidth;
    uint32_t        renderHeight;
};

// Base class for all render passes
class RenderPass {
public:
    virtual ~RenderPass() = default;

    virtual bool init(VkCtx& ctx, const Config& cfg) = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;
    virtual void record(const FrameData& frame) = 0;
    virtual void shutdown() = 0;

    // Optional: returns true if shaders were hot-reloaded
    virtual bool checkReload() { return false; }

    virtual const char* name() const = 0;
};

} // namespace sv
