// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <cstdint>

struct GLFWwindow;

namespace sv {

class VkCtx;

class ImGuiLayer {
public:
    bool init(GLFWwindow* window, VkCtx& ctx, VkFormat swapchainFormat, uint32_t imageCount);
    void shutdown(VkDevice device);

    // Call once per frame before any ImGui::* calls
    void newFrame();

    // Record ImGui draw commands into cmd.
    // swapchainImage must be in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
    // After this call it remains in COLOR_ATTACHMENT_OPTIMAL.
    void render(VkCommandBuffer cmd, VkImageView swapImageView,
                uint32_t width, uint32_t height);

    bool isInitialized() const { return m_initialized; }

private:
    VkDescriptorPool m_pool       = VK_NULL_HANDLE;
    bool             m_initialized = false;
    VkFormat         m_colorFormat = VK_FORMAT_UNDEFINED;
};

} // namespace sv
