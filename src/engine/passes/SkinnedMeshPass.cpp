// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "SkinnedMeshPass.h"
#include "../vk/VkContext.h"
#include "../vk/VkPipeline.h"
#include "../Types.h"

#include <cstdio>
#include <cstring>

namespace sv {

bool SkinnedMeshPass::init(VkCtx& ctx, VkDescriptorSetLayout sceneDescLayout,
                           VkDescriptorSetLayout bonePaletteLayout,
                           VkDescriptorSetLayout materialLayout, VkFormat colorFormat,
                           VkPipelineCache pipelineCache)
{
    m_ctx           = &ctx;
    m_colorFormat   = colorFormat;
    m_pipelineCache = pipelineCache; // may be VK_NULL_HANDLE
    VkDevice device = ctx.device();

    // ── Morph target descriptor set layout (set 3) ────────────────────
    m_morphTargetLayout = createMorphTargetDescSetLayout(device);
    if (m_morphTargetLayout == VK_NULL_HANDLE) return false;

    // ── Dummy morph SSBO for meshes without morph targets ─────────────
    {
        glm::vec4 zero(0.0f);
        m_dummyMorphSSBO = VkBuf::createWithData(ctx, &zero, sizeof(glm::vec4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolCI{};
        poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCI.maxSets       = 1;
        poolCI.poolSizeCount = 1;
        poolCI.pPoolSizes    = &poolSize;
        if (vkCreateDescriptorPool(device, &poolCI, nullptr, &m_dummyMorphPool) != VK_SUCCESS)
            return false;

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_dummyMorphPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_morphTargetLayout;
        if (vkAllocateDescriptorSets(device, &allocInfo, &m_dummyMorphDescSet) != VK_SUCCESS)
            return false;

        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_dummyMorphSSBO.buffer;
        bufInfo.offset = 0;
        bufInfo.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_dummyMorphDescSet;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo     = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    // ── Pipeline layout: set 0-3 + 128B push constants ────────────────
    VkDescriptorSetLayout setLayouts[] = {
        sceneDescLayout, bonePaletteLayout, materialLayout, m_morphTargetLayout
    };

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(SkinnedPushConstants); // 128 bytes

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 4;
    layoutCI.pSetLayouts            = setLayouts;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(device, &layoutCI, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        fprintf(stderr, "[SkinnedMeshPass] Failed to create pipeline layout\n");
        return false;
    }

    // ── Shaders ────────────────────────────────────────────────────
    if (!m_vert.loadFromFile(device, "shaders/skinned.vert", VK_SHADER_STAGE_VERTEX_BIT) ||
        !m_frag.loadFromFile(device, "shaders/skinnedPBR.frag", VK_SHADER_STAGE_FRAGMENT_BIT)) {
        fprintf(stderr, "[SkinnedMeshPass] Failed to compile shaders\n");
        return false;
    }

    // ── Graphics pipelines (pass persistent cache) ────────────────
    m_pipeline = buildSkinnedMeshPipeline(device,
        m_vert.module(), m_frag.module(), m_pipelineLayout, colorFormat,
        m_pipelineCache);
    m_pipelineBlend = buildSkinnedMeshPipelineBlend(device,
        m_vert.module(), m_frag.module(), m_pipelineLayout, colorFormat,
        m_pipelineCache);

    if (m_pipeline == VK_NULL_HANDLE || m_pipelineBlend == VK_NULL_HANDLE) {
        fprintf(stderr, "[SkinnedMeshPass] Failed to build pipeline(s)\n");
        return false;
    }

    printf("[SkinnedMeshPass] Initialized (morph target support, 4 descriptor sets, opaque+blend)\n");
    return true;
}

void SkinnedMeshPass::destroy()
{
    if (!m_ctx) return;
    VkDevice device = m_ctx->device();

    vkDestroyPipeline(device, m_pipeline, nullptr);
    vkDestroyPipeline(device, m_pipelineBlend, nullptr);
    vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    m_vert.destroy(device);
    m_frag.destroy(device);

    if (m_dummyMorphPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_dummyMorphPool, nullptr);
        m_dummyMorphPool    = VK_NULL_HANDLE;
        m_dummyMorphDescSet = VK_NULL_HANDLE;
    }
    m_dummyMorphSSBO.destroy(m_ctx->allocator());
    if (m_morphTargetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_morphTargetLayout, nullptr);
        m_morphTargetLayout = VK_NULL_HANDLE;
    }

    m_pipeline       = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_ctx            = nullptr;
}

bool SkinnedMeshPass::checkReload()
{
    VkDevice device = m_ctx->device();
    bool reloaded = false;
    reloaded |= m_vert.checkReload(device);
    reloaded |= m_frag.checkReload(device);
    if (reloaded) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
        vkDestroyPipeline(device, m_pipelineBlend, nullptr);
        m_pipeline = buildSkinnedMeshPipeline(device,
            m_vert.module(), m_frag.module(), m_pipelineLayout, m_colorFormat,
            m_pipelineCache);
        m_pipelineBlend = buildSkinnedMeshPipelineBlend(device,
            m_vert.module(), m_frag.module(), m_pipelineLayout, m_colorFormat,
            m_pipelineCache);
        printf("[SkinnedMeshPass] Pipelines rebuilt (shader hot-reload)\n");
    }
    return reloaded;
}

void SkinnedMeshPass::bind(VkCommandBuffer cmd, VkDescriptorSet sceneDescSet,
                           VkDescriptorSet bonePaletteDescSet)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkDescriptorSet sets[] = { sceneDescSet, bonePaletteDescSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout, 0, 2, sets, 0, nullptr);
}

void SkinnedMeshPass::bindBlend(VkCommandBuffer cmd, VkDescriptorSet sceneDescSet,
                                VkDescriptorSet bonePaletteDescSet)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineBlend);

    VkDescriptorSet sets[] = { sceneDescSet, bonePaletteDescSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout, 0, 2, sets, 0, nullptr);
}

void SkinnedMeshPass::draw(VkCommandBuffer cmd, const SkinnedDrawCmd& drawCmd,
                           bool alphaBlend)
{
    // Build full 128B push constants
    SkinnedPushConstants pc{};
    pc.model            = drawCmd.model;
    pc.boneOffset       = drawCmd.boneOffset;
    pc.morphTargetCount = drawCmd.morphTargetCount;
    pc.vertexCount      = drawCmd.vertexCount;
    pc.alphaMode        = alphaBlend ? 1u : 0u;
    pc.tintColor        = drawCmd.tintColor;
    pc.morphWeights0    = glm::vec4(drawCmd.morphWeights[0], drawCmd.morphWeights[1],
                                     drawCmd.morphWeights[2], drawCmd.morphWeights[3]);
    pc.morphWeights1    = glm::vec4(drawCmd.morphWeights[4], drawCmd.morphWeights[5],
                                     drawCmd.morphWeights[6], drawCmd.morphWeights[7]);

    vkCmdPushConstants(cmd, m_pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(SkinnedPushConstants), &pc);

    // Bind per-submesh material descriptor set (set 2)
    if (drawCmd.materialSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_pipelineLayout, 2, 1, &drawCmd.materialSet, 0, nullptr);
    }

    // Bind morph target descriptor set (set 3) — use dummy if none provided
    VkDescriptorSet morphSet = drawCmd.morphTargetSet != VK_NULL_HANDLE
        ? drawCmd.morphTargetSet : m_dummyMorphDescSet;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout, 3, 1, &morphSet, 0, nullptr);

    VkBuffer vertexBuffers[] = { drawCmd.vertexBuffer };
    VkDeviceSize offsets[]   = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, drawCmd.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, drawCmd.indexCount, 1, drawCmd.firstIndex, 0, 0);
}

} // namespace sv
