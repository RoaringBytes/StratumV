// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "VkTexture.h"
#include "VkContext.h"
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace sv {

// ── ColorImage ──────────────────────────────────────────────────
ColorImage ColorImage::create(VkCtx& ctx, uint32_t w, uint32_t h, VkFormat format,
    VkImageUsageFlags extraUsage)
{
    ColorImage ci;
    ci.width  = w;
    ci.height = h;

    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = format;
    imgCI.extent        = { w, h, 1 };
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                        | VK_IMAGE_USAGE_SAMPLED_BIT
                        | extraUsage;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(ctx.allocator(), &imgCI, &allocCI,
            &ci.image, &ci.allocation, nullptr) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create color image\n");
        return ci;
    }

    VkImageViewCreateInfo viewCI{};
    viewCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image    = ci.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format   = format;
    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.levelCount = 1;
    viewCI.subresourceRange.layerCount = 1;

    if (vkCreateImageView(ctx.device(), &viewCI, nullptr, &ci.view) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create color image view\n");
    }

    VkSamplerCreateInfo sampCI{};
    sampCI.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter    = VK_FILTER_LINEAR;
    sampCI.minFilter    = VK_FILTER_LINEAR;
    sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.maxLod       = 1.0f;

    if (vkCreateSampler(ctx.device(), &sampCI, nullptr, &ci.sampler) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create color image sampler\n");
    }

    // Transition to SHADER_READ_ONLY (render loop will transition to COLOR_ATTACHMENT before writing)
    auto cmd = ctx.beginSingleTimeCommands();
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = ci.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    ctx.endSingleTimeCommands(cmd);

    printf("[StratumV] Color image: %ux%u\n", w, h);
    return ci;
}

void ColorImage::destroy(VkDevice device, VmaAllocator alloc)
{
    if (sampler) vkDestroySampler(device, sampler, nullptr);
    if (view)    vkDestroyImageView(device, view, nullptr);
    if (image)   vmaDestroyImage(alloc, image, allocation);
    sampler    = VK_NULL_HANDLE;
    view       = VK_NULL_HANDLE;
    image      = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
}

// ── DepthImage ──────────────────────────────────────────────────
DepthImage DepthImage::create(VkCtx& ctx, uint32_t width, uint32_t height, VkFormat format)
{
    DepthImage d;

    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = format;
    imgCI.extent        = { width, height, 1 };
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        | VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(ctx.allocator(), &imgCI, &allocCI,
            &d.image, &d.allocation, nullptr) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create depth image\n");
        return d;
    }

    VkImageViewCreateInfo viewCI{};
    viewCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image    = d.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format   = format;
    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewCI.subresourceRange.levelCount = 1;
    viewCI.subresourceRange.layerCount = 1;

    if (vkCreateImageView(ctx.device(), &viewCI, nullptr, &d.view) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create depth image view\n");
    }

    // Transition to depth attachment layout
    auto cmd = ctx.beginSingleTimeCommands();
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = d.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    ctx.endSingleTimeCommands(cmd);

    printf("[StratumV] Depth image: %ux%u\n", width, height);
    return d;
}

void DepthImage::destroy(VkDevice device, VmaAllocator alloc)
{
    if (view)  vkDestroyImageView(device, view, nullptr);
    if (image) vmaDestroyImage(alloc, image, allocation);
    view       = VK_NULL_HANDLE;
    image      = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
}

// ── ShadowMap ───────────────────────────────────────────────────
ShadowMap ShadowMap::create(VkCtx& ctx, uint32_t sz)
{
    ShadowMap sm;
    sm.size = sz;
    VkFormat format = VK_FORMAT_D32_SFLOAT;

    // Create layered depth image (one layer per cascade)
    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = format;
    imgCI.extent        = { sz, sz, 1 };
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = CASCADE_COUNT;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        | VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(ctx.allocator(), &imgCI, &allocCI,
            &sm.image, &sm.allocation, nullptr) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create shadow map image\n");
        return sm;
    }

    // 2D_ARRAY view for sampling all cascades from fragment shaders
    VkImageViewCreateInfo viewCI{};
    viewCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image    = sm.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewCI.format   = format;
    viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewCI.subresourceRange.baseMipLevel   = 0;
    viewCI.subresourceRange.levelCount     = 1;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount     = CASCADE_COUNT;

    if (vkCreateImageView(ctx.device(), &viewCI, nullptr, &sm.view) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create shadow map array view\n");
    }

    // Per-layer 2D views for rendering into individual cascade layers
    for (uint32_t i = 0; i < CASCADE_COUNT; i++) {
        VkImageViewCreateInfo layerCI{};
        layerCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        layerCI.image    = sm.image;
        layerCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        layerCI.format   = format;
        layerCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        layerCI.subresourceRange.baseMipLevel   = 0;
        layerCI.subresourceRange.levelCount     = 1;
        layerCI.subresourceRange.baseArrayLayer = i;
        layerCI.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(ctx.device(), &layerCI, nullptr, &sm.layerViews[i]) != VK_SUCCESS) {
            fprintf(stderr, "[StratumV] Failed to create shadow map layer %u view\n", i);
        }
    }

    // Comparison sampler for hardware PCF (sampler2DArrayShadow)
    VkSamplerCreateInfo sampCI{};
    sampCI.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter    = VK_FILTER_LINEAR;
    sampCI.minFilter    = VK_FILTER_LINEAR;
    sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // outside = fully lit
    sampCI.compareEnable = VK_TRUE;
    sampCI.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;
    sampCI.maxLod        = 1.0f;

    if (vkCreateSampler(ctx.device(), &sampCI, nullptr, &sm.sampler) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create shadow map sampler\n");
    }

    // Transition all layers to SHADER_READ_ONLY
    auto cmd = ctx.beginSingleTimeCommands();
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = sm.image;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = CASCADE_COUNT;
    barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    ctx.endSingleTimeCommands(cmd);

    printf("[StratumV] Shadow map: %ux%u x %u cascades\n", sz, sz, CASCADE_COUNT);
    return sm;
}

void ShadowMap::destroy(VkDevice device, VmaAllocator alloc)
{
    if (sampler) vkDestroySampler(device, sampler, nullptr);
    if (view)    vkDestroyImageView(device, view, nullptr);
    for (uint32_t i = 0; i < CASCADE_COUNT; i++) {
        if (layerViews[i]) vkDestroyImageView(device, layerViews[i], nullptr);
        layerViews[i] = VK_NULL_HANDLE;
    }
    if (image) vmaDestroyImage(alloc, image, allocation);
    sampler    = VK_NULL_HANDLE;
    view       = VK_NULL_HANDLE;
    image      = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
}

// ── StorageImage ────────────────────────────────────────────────
StorageImage StorageImage::create(VkCtx& ctx, uint32_t w, uint32_t h,
    VkFormat format, uint32_t numLayers)
{
    StorageImage si;
    si.width  = w;
    si.height = h;
    si.layers = numLayers;

    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = format;
    imgCI.extent        = { w, h, 1 };
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = numLayers;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_STORAGE_BIT
                        | VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(ctx.allocator(), &imgCI, &allocCI,
            &si.image, &si.allocation, nullptr) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create storage image\n");
        return si;
    }

    // Full view (2D or 2D_ARRAY depending on layer count)
    VkImageViewCreateInfo viewCI{};
    viewCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image    = si.image;
    viewCI.viewType = (numLayers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format   = format;
    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.levelCount = 1;
    viewCI.subresourceRange.layerCount = numLayers;

    if (vkCreateImageView(ctx.device(), &viewCI, nullptr, &si.view) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create storage image view\n");
    }

    // Per-layer views (for compute imageStore to specific layers)
    if (numLayers > 1) {
        for (uint32_t layer = 0; layer < numLayers && layer < 8; layer++) {
            VkImageViewCreateInfo layerViewCI{};
            layerViewCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            layerViewCI.image    = si.image;
            layerViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
            layerViewCI.format   = format;
            layerViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            layerViewCI.subresourceRange.levelCount = 1;
            layerViewCI.subresourceRange.baseArrayLayer = layer;
            layerViewCI.subresourceRange.layerCount = 1;

            if (vkCreateImageView(ctx.device(), &layerViewCI, nullptr, &si.layerViews[layer]) != VK_SUCCESS) {
                fprintf(stderr, "[StratumV] Failed to create storage image layer view %u\n", layer);
            }
        }
    }

    VkSamplerCreateInfo sampCI{};
    sampCI.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter    = VK_FILTER_LINEAR;
    sampCI.minFilter    = VK_FILTER_LINEAR;
    sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampCI.maxLod       = 1.0f;

    if (vkCreateSampler(ctx.device(), &sampCI, nullptr, &si.sampler) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create storage image sampler\n");
    }

    // Transition to GENERAL (compute shaders read/write in GENERAL layout)
    auto cmd = ctx.beginSingleTimeCommands();
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = si.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = numLayers;
    barrier.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    ctx.endSingleTimeCommands(cmd);

    printf("[StratumV] Storage image: %ux%u x%u layers\n", w, h, numLayers);
    return si;
}

void StorageImage::destroy(VkDevice device, VmaAllocator alloc)
{
    if (sampler) vkDestroySampler(device, sampler, nullptr);
    if (view)    vkDestroyImageView(device, view, nullptr);
    for (uint32_t i = 0; i < layers && i < 8; i++) {
        if (layerViews[i]) vkDestroyImageView(device, layerViews[i], nullptr);
        layerViews[i] = VK_NULL_HANDLE;
    }
    if (image)   vmaDestroyImage(alloc, image, allocation);
    sampler    = VK_NULL_HANDLE;
    view       = VK_NULL_HANDLE;
    image      = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    layers     = 1;
}

// ── VkTex ───────────────────────────────────────────────────────
bool VkTex::loadFromFile(VkCtx& ctx, const std::string& path, bool srgb)
{
    int w, h, channels;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) {
        fprintf(stderr, "[StratumV] Failed to load texture: %s\n", path.c_str());
        return false;
    }

    m_width  = (uint32_t)w;
    m_height = (uint32_t)h;
    bool ok = createGPUImage(ctx, pixels, srgb);
    stbi_image_free(pixels);

    if (ok) printf("[StratumV] Texture loaded: %s (%ux%u, %u mips)\n",
        path.c_str(), m_width, m_height, m_mipLevels);
    return ok;
}

bool VkTex::loadFromMemory(VkCtx& ctx, const uint8_t* pixels,
    uint32_t width, uint32_t height, bool srgb)
{
    m_width  = width;
    m_height = height;
    return createGPUImage(ctx, pixels, srgb);
}

void VkTex::destroy(VkDevice device, VmaAllocator alloc)
{
    if (m_sampler) vkDestroySampler(device, m_sampler, nullptr);
    if (m_view)    vkDestroyImageView(device, m_view, nullptr);
    if (m_image)   vmaDestroyImage(alloc, m_image, m_allocation);
    m_sampler    = VK_NULL_HANDLE;
    m_view       = VK_NULL_HANDLE;
    m_image      = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
}

bool VkTex::createGPUImage(VkCtx& ctx, const uint8_t* pixels, bool srgb)
{
    VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkDeviceSize imageSize = (VkDeviceSize)m_width * m_height * 4;
    m_mipLevels = (uint32_t)std::floor(std::log2((std::max)(m_width, m_height))) + 1;

    // Create staging buffer
    VkBuffer stagingBuffer;
    VmaAllocation stagingAlloc;
    {
        VkBufferCreateInfo bufCI{};
        bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufCI.size  = imageSize;
        bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo allocCI{};
        allocCI.usage = VMA_MEMORY_USAGE_CPU_ONLY;

        vmaCreateBuffer(ctx.allocator(), &bufCI, &allocCI,
            &stagingBuffer, &stagingAlloc, nullptr);

        void* mapped;
        vmaMapMemory(ctx.allocator(), stagingAlloc, &mapped);
        memcpy(mapped, pixels, (size_t)imageSize);
        vmaUnmapMemory(ctx.allocator(), stagingAlloc);
    }

    // Create image
    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = format;
    imgCI.extent        = { m_width, m_height, 1 };
    imgCI.mipLevels     = m_mipLevels;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(ctx.allocator(), &imgCI, &allocCI,
            &m_image, &m_allocation, nullptr) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create texture image\n");
        vmaDestroyBuffer(ctx.allocator(), stagingBuffer, stagingAlloc);
        return false;
    }

    // Transition mip 0 to TRANSFER_DST, copy staging → image, then generate mipmaps
    auto cmd = ctx.beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = m_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = m_mipLevels;
    barrier.subresourceRange.layerCount     = 1;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { m_width, m_height, 1 };

    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    ctx.endSingleTimeCommands(cmd);

    // Generate mipmaps (transitions all levels to SHADER_READ_ONLY)
    generateMipmaps(ctx, format);

    vmaDestroyBuffer(ctx.allocator(), stagingBuffer, stagingAlloc);

    // Create image view
    VkImageViewCreateInfo viewCI{};
    viewCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image    = m_image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format   = format;
    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.levelCount = m_mipLevels;
    viewCI.subresourceRange.layerCount = 1;

    if (vkCreateImageView(ctx.device(), &viewCI, nullptr, &m_view) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create texture image view\n");
        return false;
    }

    // Create sampler
    VkSamplerCreateInfo sampCI{};
    sampCI.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter    = VK_FILTER_LINEAR;
    sampCI.minFilter    = VK_FILTER_LINEAR;
    sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampCI.anisotropyEnable = VK_TRUE;
    sampCI.maxAnisotropy    = 16.0f;
    sampCI.maxLod           = (float)m_mipLevels;

    if (vkCreateSampler(ctx.device(), &sampCI, nullptr, &m_sampler) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create texture sampler\n");
        return false;
    }

    return true;
}

void VkTex::generateMipmaps(VkCtx& ctx, VkFormat format)
{
    auto cmd = ctx.beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = m_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    int32_t mipW = (int32_t)m_width;
    int32_t mipH = (int32_t)m_height;

    for (uint32_t i = 1; i < m_mipLevels; i++) {
        // Transition previous level to TRANSFER_SRC
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel   = i - 1;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = { mipW, mipH, 1 };

        int32_t nextW = mipW > 1 ? mipW / 2 : 1;
        int32_t nextH = mipH > 1 ? mipH / 2 : 1;

        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel   = i;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1] = { nextW, nextH, 1 };

        vkCmdBlitImage(cmd,
            m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        // Transition previous level to SHADER_READ_ONLY
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        mipW = nextW;
        mipH = nextH;
    }

    // Transition last mip level to SHADER_READ_ONLY
    barrier.subresourceRange.baseMipLevel = m_mipLevels - 1;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    ctx.endSingleTimeCommands(cmd);
}

} // namespace sv
