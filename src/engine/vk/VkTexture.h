// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>
#include <string>
#include <cstdint>

namespace sv {

class VkCtx;

// GPU color image (for HDR render targets, post-process)
struct ColorImage {
    VkImage       image      = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VkSampler     sampler    = VK_NULL_HANDLE;
    uint32_t      width      = 0;
    uint32_t      height     = 0;

    static ColorImage create(VkCtx& ctx, uint32_t w, uint32_t h,
        VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT,
        VkImageUsageFlags extraUsage = 0);
    void destroy(VkDevice device, VmaAllocator alloc);
};

// GPU depth image (for depth attachment)
struct DepthImage {
    VkImage       image      = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;

    static DepthImage create(VkCtx& ctx, uint32_t width, uint32_t height,
        VkFormat format = VK_FORMAT_D32_SFLOAT);
    void destroy(VkDevice device, VmaAllocator alloc);
};

// GPU shadow map (layered depth array + comparison sampler for cascaded shadow mapping)
struct ShadowMap {
    static constexpr uint32_t CASCADE_COUNT = 3;

    VkImage       image      = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;          // 2D_ARRAY view (for sampling all cascades)
    VkImageView   layerViews[CASCADE_COUNT] = {};        // per-layer 2D views (for rendering)
    VkSampler     sampler    = VK_NULL_HANDLE;
    uint32_t      size       = 0;

    static ShadowMap create(VkCtx& ctx, uint32_t size);
    void destroy(VkDevice device, VmaAllocator alloc);
};

// GPU storage image (for compute shader read/write, also sampleable)
// When layers > 1, view is a 2D_ARRAY and layerViews[] has per-layer views for compute writes
struct StorageImage {
    VkImage       image      = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;   // Full view (2D or 2D_ARRAY)
    VkSampler     sampler    = VK_NULL_HANDLE;    // LINEAR, REPEAT (for graphics sampling)
    uint32_t      width      = 0;
    uint32_t      height     = 0;
    uint32_t      layers     = 1;
    VkImageView   layerViews[8] = {};             // Per-layer views (for compute imageStore to specific layer)

    static StorageImage create(VkCtx& ctx, uint32_t w, uint32_t h,
        VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t layers = 1);
    void destroy(VkDevice device, VmaAllocator alloc);
};

// GPU texture loaded from file
class VkTex {
public:
    bool loadFromFile(VkCtx& ctx, const std::string& path, bool srgb = true);
    bool loadFromMemory(VkCtx& ctx, const uint8_t* pixels,
        uint32_t width, uint32_t height, bool srgb = true);
    void destroy(VkDevice device, VmaAllocator alloc);

    VkImage     image()   const { return m_image; }    // blit source
    VkImageView view()    const { return m_view; }
    VkSampler   sampler() const { return m_sampler; }
    uint32_t    width()   const { return m_width; }
    uint32_t    height()  const { return m_height; }
    uint32_t    mipLevels() const { return m_mipLevels; }

private:
    bool createGPUImage(VkCtx& ctx, const uint8_t* pixels, bool srgb);
    void generateMipmaps(VkCtx& ctx, VkFormat format);

    VkImage       m_image      = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkImageView   m_view       = VK_NULL_HANDLE;
    VkSampler     m_sampler    = VK_NULL_HANDLE;
    uint32_t      m_width      = 0;
    uint32_t      m_height     = 0;
    uint32_t      m_mipLevels  = 1;
};

} // namespace sv
