// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include "vk/VkTexture.h"
#include "vk/VkDescriptors.h"
#include "vk/VkShader.h"
#include "vk/VkBuffer.h"

namespace sv {

class VkCtx;

struct PostProcessUBO {
    float bloomThreshold;   // 0
    float bloomIntensity;   // 4
    float exposure;         // 8
    float gamma;            // 12
};                          // 16 bytes

class PostProcess {
public:
    bool init(VkCtx& ctx, uint32_t width, uint32_t height, VkFormat swapFormat);
    void shutdown(VkDevice device, VmaAllocator alloc);
    void resize(VkCtx& ctx, uint32_t width, uint32_t height);

    // Record bloom + tonemap commands.
    // Call after HDR pass ends and hdrColor is in SHADER_READ_ONLY layout.
    // swapImage must be in UNDEFINED layout (will be transitioned internally).
    void recordCommands(VkCommandBuffer cmd,
        VkImage swapImage, VkImageView swapImageView, VkExtent2D swapExtent);

    // The HDR render target (scene renders into this)
    ColorImage& hdrTarget() { return m_hdrColor; }
    VkFormat    hdrFormat() const { return VK_FORMAT_R16G16B16A16_SFLOAT; }

    // Update UBO each frame
    void updateParams(const PostProcessUBO& params);

    // Override HDR source for DLSS (rewrites threshold + tonemap descriptors)
    void setHdrSource(VkDevice device, VkImageView view, VkSampler sampler);

    // Restore HDR source to internal m_hdrColor
    void resetHdrSource(VkDevice device);

    // Hot-reload support — returns true if pipelines were rebuilt
    bool checkReload(VkDevice device);

    static constexpr int BLOOM_PASSES = 5;

private:
    void createImages(VkCtx& ctx, uint32_t w, uint32_t h);
    void destroyImages(VkDevice device, VmaAllocator alloc);
    void createDescriptors(VkDevice device);
    void writeDescriptors(VkDevice device);
    void createPipelines(VkDevice device);
    void destroyPipelines(VkDevice device);

    VkCtx* m_ctx = nullptr;
    VkFormat m_swapFormat = VK_FORMAT_B8G8R8A8_SRGB;
    uint32_t m_width = 0, m_height = 0;

    // Images
    ColorImage m_hdrColor;           // Full-res HDR render target
    ColorImage m_brightPass;         // Half-res bright extraction
    ColorImage m_blur[2];            // Half-res blur ping-pong

    // Shaders
    VkShader m_ppVert;               // postprocess.vert
    VkShader m_threshFrag;           // bloom_threshold.frag
    VkShader m_blurFrag;             // bloom_blur.frag
    VkShader m_tonemapFrag;          // tonemap.frag

    // Descriptor layouts
    VkDescriptorSetLayout m_threshLayout  = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_blurLayout    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_tonemapLayout = VK_NULL_HANDLE;

    // Pipeline layouts
    VkPipelineLayout m_threshPipeLayout  = VK_NULL_HANDLE;
    VkPipelineLayout m_blurPipeLayout    = VK_NULL_HANDLE;
    VkPipelineLayout m_tonemapPipeLayout = VK_NULL_HANDLE;

    // Pipelines
    VkPipeline m_threshPipeline  = VK_NULL_HANDLE;
    VkPipeline m_blurPipeline    = VK_NULL_HANDLE;
    VkPipeline m_tonemapPipeline = VK_NULL_HANDLE;

    // Descriptor pool + sets
    VkDescPool m_descPool;
    VkDescriptorSet m_threshDesc   = VK_NULL_HANDLE;
    VkDescriptorSet m_blurDesc[3]  = {};   // [0]=brightPass, [1]=blur[0], [2]=blur[1]
    VkDescriptorSet m_tonemapDesc  = VK_NULL_HANDLE;

    // PostProcess UBO
    VkBuf m_ubo;
};

} // namespace sv
