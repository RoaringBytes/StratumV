// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "ShadowPass.h"
#include "../vk/VkContext.h"
#include "../vk/VkPipeline.h"
#include "../Config.h"
#include "../Types.h"

namespace sv {

bool ShadowPass::init(VkCtx& ctx, const Config& cfg)
{
    m_ctx = &ctx;
    VkDevice device = ctx.device();

    m_mapSize = cfg.get<uint32_t>("shadow.mapSize", 4096);

    // Shadow map (depth-only, CASCADE_COUNT layers)
    m_shadowMap = ShadowMap::create(ctx, m_mapSize);

    // Shaders
    if (!m_vert.loadFromFile(device, "shaders/shadow.vert", VK_SHADER_STAGE_VERTEX_BIT) ||
        !m_frag.loadFromFile(device, "shaders/shadow.frag", VK_SHADER_STAGE_FRAGMENT_BIT)) {
        fprintf(stderr, "[StratumV] ShadowPass: failed to compile shaders\n");
        return false;
    }

    // Skinned shadow shader (optional — loaded if file exists)
    m_skinnedVert.loadFromFile(device, "shaders/skinnedShadow.vert", VK_SHADER_STAGE_VERTEX_BIT);

    printf("[StratumV] ShadowPass initialized (%ux%u, %u cascades)\n",
        m_mapSize, m_mapSize, ShadowMap::CASCADE_COUNT);
    return true;
}

void ShadowPass::buildPipeline()
{
    m_pipeline = sv::buildShadowPipeline(m_ctx->device(),
        m_vert.module(), m_frag.module(), m_pipelineLayout);
}

void ShadowPass::buildSkinnedPipeline()
{
    if (!m_skinnedVert.module() || m_skinnedPipeLayout == VK_NULL_HANDLE) return;
    m_skinnedPipeline = sv::buildSkinnedShadowPipeline(m_ctx->device(),
        m_skinnedVert.module(), m_frag.module(), m_skinnedPipeLayout);
}

void ShadowPass::setSkinnedBindings(const SkinnedShadowBinding* bindings, uint32_t count)
{
    m_skinnedBindings.assign(bindings, bindings + count);
}

void ShadowPass::resize(uint32_t /*width*/, uint32_t /*height*/)
{
    // Shadow map resolution is fixed, not tied to window size
}

void ShadowPass::record(const FrameData& frame)
{
    if (m_mesh.indexCount == 0) return;

    VkCommandBuffer cmd = frame.cmd;

    // Transition all cascade layers: SHADER_READ_ONLY → DEPTH_STENCIL_ATTACHMENT
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = m_shadowMap.image;
        barrier.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, ShadowMap::CASCADE_COUNT };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    for (uint32_t cascade = 0; cascade < ShadowMap::CASCADE_COUNT; cascade++) {
        VkRenderingAttachmentInfo shadowDepth{};
        shadowDepth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        shadowDepth.imageView   = m_shadowMap.layerViews[cascade];
        shadowDepth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        shadowDepth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        shadowDepth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        shadowDepth.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo renderInfo{};
        renderInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea.extent    = { m_shadowMap.size, m_shadowMap.size };
        renderInfo.layerCount           = 1;
        renderInfo.colorAttachmentCount = 0;
        renderInfo.pDepthAttachment     = &shadowDepth;

        vkCmdBeginRendering(cmd, &renderInfo);

        VkViewport vp{};
        vp.width    = (float)m_shadowMap.size;
        vp.height   = (float)m_shadowMap.size;
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D sc{};
        sc.extent = { m_shadowMap.size, m_shadowMap.size };
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_pipelineLayout, 0, 1, &frame.sceneDescSet, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

        glm::mat4 model(1.0f);
        vkCmdPushConstants(cmd, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model);
        vkCmdPushConstants(cmd, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4), sizeof(uint32_t), &cascade);

        VkBuffer vertexBuffers[] = { m_mesh.vertexBuffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, m_mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m_mesh.indexCount, 1, 0, 0, 0);

        // ── Skinned meshes ─────────────────────────────────────────
        if (m_skinnedPipeline && !m_skinnedBindings.empty() && m_bonePaletteDescSet) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);

            VkDescriptorSet sets[] = { frame.sceneDescSet, m_bonePaletteDescSet };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_skinnedPipeLayout, 0, 2, sets, 0, nullptr);

            for (const auto& skin : m_skinnedBindings) {
                vkCmdPushConstants(cmd, m_skinnedPipeLayout,
                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &skin.model);
                vkCmdPushConstants(cmd, m_skinnedPipeLayout,
                    VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4), sizeof(uint32_t), &cascade);
                vkCmdPushConstants(cmd, m_skinnedPipeLayout,
                    VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4) + sizeof(uint32_t),
                    sizeof(uint32_t), &skin.boneOffset);

                VkBuffer skinVB[] = { skin.vertexBuffer };
                vkCmdBindVertexBuffers(cmd, 0, 1, skinVB, offsets);
                vkCmdBindIndexBuffer(cmd, skin.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, skin.indexCount, 1, 0, 0, 0);
            }

            // Re-bind static pipeline if more cascades follow
            if (cascade + 1 < ShadowMap::CASCADE_COUNT && m_mesh.indexCount > 0) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
            }
        }

        vkCmdEndRendering(cmd);
    }

    // Transition all cascade layers back: DEPTH_STENCIL_ATTACHMENT → SHADER_READ_ONLY
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = m_shadowMap.image;
        barrier.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, ShadowMap::CASCADE_COUNT };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
}

bool ShadowPass::checkReload()
{
    VkDevice device = m_ctx->device();
    bool reloaded = false;
    reloaded |= m_vert.checkReload(device);
    reloaded |= m_frag.checkReload(device);
    if (reloaded) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
        m_pipeline = buildShadowPipeline(device,
            m_vert.module(), m_frag.module(), m_pipelineLayout);
        printf("[StratumV] ShadowPass: pipelines rebuilt\n");
    }

    // Skinned shadow shader hot-reload
    bool skinnedReloaded = m_skinnedVert.checkReload(device);
    if ((reloaded || skinnedReloaded) && m_skinnedPipeline) {
        vkDestroyPipeline(device, m_skinnedPipeline, nullptr);
        m_skinnedPipeline = buildSkinnedShadowPipeline(device,
            m_skinnedVert.module(), m_frag.module(), m_skinnedPipeLayout);
        printf("[StratumV] ShadowPass: skinned pipeline rebuilt\n");
    }

    return reloaded || skinnedReloaded;
}

void ShadowPass::shutdown()
{
    if (!m_ctx) return;
    VkDevice device = m_ctx->device();
    VmaAllocator alloc = m_ctx->allocator();

    vkDestroyPipeline(device, m_pipeline, nullptr);
    vkDestroyPipeline(device, m_skinnedPipeline, nullptr);
    m_shadowMap.destroy(device, alloc);
    m_vert.destroy(device);
    m_frag.destroy(device);
    m_skinnedVert.destroy(device);
    m_skinnedBindings.clear();
    m_ctx = nullptr;
}

} // namespace sv
