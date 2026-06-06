// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "VkSwapchain.h"
#include "VkContext.h"
#include <cstdio>
#include <algorithm>

namespace sv {

bool VkSwap::init(VkCtx& ctx, uint32_t width, uint32_t height, bool vsync)
{
    VkPhysicalDevice gpu = ctx.physicalDevice();
    VkDevice device = ctx.device();
    VkSurfaceKHR surface = ctx.surface();

    // Query surface capabilities
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);

    // Query formats
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, formats.data());

    // Query present modes
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &modeCount, modes.data());

    auto surfaceFormat = chooseSurfaceFormat(formats);
    auto presentMode = choosePresentMode(modes, vsync);
    auto extent = chooseExtent(caps, width, height);

    // Request one more than minimum for triple buffering
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    auto& families = ctx.queueFamilies();
    uint32_t queueFamilyIndices[] = { families.graphics, families.present };

    if (families.graphics != families.present) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = caps.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = m_swapchain; // For recreation

    if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create swapchain\n");
        return false;
    }

    m_format = surfaceFormat.format;
    m_extent = extent;

    // Get swapchain images
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, m_swapchain, &count, nullptr);
    m_images.resize(count);
    vkGetSwapchainImagesKHR(device, m_swapchain, &count, m_images.data());

    createImageViews(device);

    printf("[StratumV] Swapchain: %ux%u, %u images, format %d\n",
        extent.width, extent.height, count, surfaceFormat.format);

    return true;
}

void VkSwap::shutdown(VkDevice device)
{
    for (auto view : m_imageViews)
        vkDestroyImageView(device, view, nullptr);
    m_imageViews.clear();

    if (m_swapchain) {
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

bool VkSwap::recreate(VkCtx& ctx, uint32_t width, uint32_t height, bool vsync)
{
    vkDeviceWaitIdle(ctx.device());

    // Destroy old image views (swapchain itself is recycled via oldSwapchain)
    for (auto view : m_imageViews)
        vkDestroyImageView(ctx.device(), view, nullptr);
    m_imageViews.clear();

    VkSwapchainKHR old = m_swapchain;
    bool ok = init(ctx, width, height, vsync);

    // Destroy the old swapchain after creating the new one
    if (old != VK_NULL_HANDLE && old != m_swapchain)
        vkDestroySwapchainKHR(ctx.device(), old, nullptr);

    return ok;
}

// ── Private ──────────────────────────────────────────────────────
VkSurfaceFormatKHR VkSwap::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
{
    // Prefer SRGB for correct gamma
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    }
    return formats[0];
}

VkPresentModeKHR VkSwap::choosePresentMode(const std::vector<VkPresentModeKHR>& modes, bool vsync)
{
    if (!vsync) {
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR; // Always available, vsync
}

VkExtent2D VkSwap::chooseExtent(const VkSurfaceCapabilitiesKHR& caps, uint32_t w, uint32_t h)
{
    if (caps.currentExtent.width != UINT32_MAX)
        return caps.currentExtent;

    VkExtent2D extent = { w, h };
    extent.width  = std::clamp(extent.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return extent;
}

void VkSwap::createImageViews(VkDevice device)
{
    m_imageViews.resize(m_images.size());
    for (size_t i = 0; i < m_images.size(); i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &m_imageViews[i]) != VK_SUCCESS) {
            fprintf(stderr, "[StratumV] Failed to create swapchain image view %zu\n", i);
        }
    }
}

} // namespace sv
