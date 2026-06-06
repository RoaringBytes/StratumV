// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <vector>
#include <cstdint>

namespace sv {

class VkCtx;

class VkSwap {
public:
    bool init(VkCtx& ctx, uint32_t width, uint32_t height, bool vsync = true);
    void shutdown(VkDevice device);
    bool recreate(VkCtx& ctx, uint32_t width, uint32_t height, bool vsync = true);

    VkSwapchainKHR   swapchain()  const { return m_swapchain; }
    VkFormat         format()     const { return m_format; }
    VkExtent2D       extent()     const { return m_extent; }
    uint32_t         imageCount() const { return (uint32_t)m_imageViews.size(); }
    VkImageView      imageView(uint32_t i) const { return m_imageViews[i]; }
    VkImage          image(uint32_t i)     const { return m_images[i]; }

private:
    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);
    VkPresentModeKHR   choosePresentMode(const std::vector<VkPresentModeKHR>& modes, bool vsync);
    VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR& caps, uint32_t w, uint32_t h);
    void createImageViews(VkDevice device);

    VkSwapchainKHR           m_swapchain = VK_NULL_HANDLE;
    VkFormat                 m_format    = VK_FORMAT_UNDEFINED;
    VkExtent2D               m_extent    = {0, 0};
    std::vector<VkImage>     m_images;
    std::vector<VkImageView> m_imageViews;
};

} // namespace sv
