// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#ifdef _WIN32
#define NOMINMAX
#endif
#include "ImGuiLayer.h"
#include "UiStyle.h"
#include "../vk/VkContext.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>

#include <cstdio>

namespace sv {

bool ImGuiLayer::init(GLFWwindow* window, VkCtx& ctx, VkFormat swapchainFormat, uint32_t imageCount)
{
    m_colorFormat = swapchainFormat;

    // Descriptor pool dedicated to ImGui (generous sizes — it manages its own allocation)
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER,                1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1000 },
    };

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = 1000;
    poolCI.poolSizeCount = (uint32_t)std::size(poolSizes);
    poolCI.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(ctx.device(), &poolCI, nullptr, &m_pool) != VK_SUCCESS) {
        fprintf(stderr, "[ImGui] Failed to create descriptor pool\n");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Engine default theme (colors, geometry, font)
    sv::style::applyDefaultStyle();
    sv::style::loadStyleFromJSON("data/ui_style.json");

    // GLFW backend
    ImGui_ImplGlfw_InitForVulkan(window, /*install_callbacks=*/true);

    // Vulkan backend with dynamic rendering
    VkPipelineRenderingCreateInfo pipelineRI{};
    pipelineRI.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipelineRI.colorAttachmentCount    = 1;
    pipelineRI.pColorAttachmentFormats = &m_colorFormat;

    ImGui_ImplVulkan_InitInfo vkInfo{};
    vkInfo.Instance                    = ctx.instance();
    vkInfo.PhysicalDevice              = ctx.physicalDevice();
    vkInfo.Device                      = ctx.device();
    vkInfo.QueueFamily                 = ctx.queueFamilies().graphics;
    vkInfo.Queue                       = ctx.graphicsQueue();
    vkInfo.DescriptorPool              = m_pool;
    vkInfo.MinImageCount               = 2;
    vkInfo.ImageCount                  = imageCount;
    vkInfo.ApiVersion                  = VK_API_VERSION_1_3;
    vkInfo.UseDynamicRendering         = true;
    // Since ImGui 1.92: pipeline state lives in PipelineInfoMain
    vkInfo.PipelineInfoMain.MSAASamples                 = VK_SAMPLE_COUNT_1_BIT;
    vkInfo.PipelineInfoMain.PipelineRenderingCreateInfo = pipelineRI;

    if (!ImGui_ImplVulkan_Init(&vkInfo)) {
        fprintf(stderr, "[ImGui] Failed to init Vulkan backend\n");
        return false;
    }

    // Since ImGui 1.92 font/texture upload is handled automatically by the
    // backend (ImGuiBackendFlags_RendererHasTextures); no explicit call needed.

    m_initialized = true;
    printf("[ImGui] Initialized (dynamic rendering, format=%d, images=%u)\n",
           (int)swapchainFormat, imageCount);
    return true;
}

void ImGuiLayer::shutdown(VkDevice device)
{
    if (!m_initialized) return;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    m_initialized = false;
}

void ImGuiLayer::newFrame()
{
    if (!m_initialized) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::render(VkCommandBuffer cmd, VkImageView swapImageView,
                        uint32_t width, uint32_t height)
{
    if (!m_initialized) return;

    ImGui::Render();

    // Dynamic rendering attachment — LOAD_OP_LOAD to composite over scene
    VkRenderingAttachmentInfo colorAttach{};
    colorAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttach.imageView   = swapImageView;
    colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo renderInfo{};
    renderInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea.extent    = { width, height };
    renderInfo.layerCount           = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments    = &colorAttach;

    vkCmdBeginRendering(cmd, &renderInfo);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

} // namespace sv
