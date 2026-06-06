// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "PostProcess.h"
#include "vk/VkContext.h"
#include "vk/VkPipeline.h"
#include <glm/glm.hpp>
#include <cstdio>

namespace sv {

// ── Image layout transition (local helper) ──────────────────────
static void transitionImage(VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkAccessFlags srcAccess, VkAccessFlags dstAccess,
    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask       = srcAccess;
    barrier.dstAccessMask       = dstAccess;
    barrier.oldLayout           = oldLayout;
    barrier.newLayout           = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image;
    barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0,
        0, nullptr, 0, nullptr, 1, &barrier);
}

// ── Init ────────────────────────────────────────────────────────
bool PostProcess::init(VkCtx& ctx, uint32_t width, uint32_t height, VkFormat swapFormat)
{
    m_ctx        = &ctx;
    m_swapFormat = swapFormat;
    m_width      = width;
    m_height     = height;

    VkDevice device = ctx.device();

    // Load shaders
    if (!m_ppVert.loadFromFile(device, "shaders/postprocess.vert", VK_SHADER_STAGE_VERTEX_BIT) ||
        !m_threshFrag.loadFromFile(device, "shaders/bloom_threshold.frag", VK_SHADER_STAGE_FRAGMENT_BIT) ||
        !m_blurFrag.loadFromFile(device, "shaders/bloom_blur.frag", VK_SHADER_STAGE_FRAGMENT_BIT) ||
        !m_tonemapFrag.loadFromFile(device, "shaders/tonemap.frag", VK_SHADER_STAGE_FRAGMENT_BIT))
    {
        fprintf(stderr, "[StratumV] Failed to load post-process shaders\n");
        return false;
    }

    // Create UBO (CPU_TO_GPU for per-frame updates)
    PostProcessUBO defaults{};
    defaults.bloomThreshold = 2.0f;
    defaults.bloomIntensity = 0.15f;
    defaults.exposure       = 0.85f;
    defaults.gamma          = 2.2f;

    m_ubo = VkBuf::create(ctx.allocator(), sizeof(PostProcessUBO),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    memcpy(m_ubo.info.pMappedData, &defaults, sizeof(PostProcessUBO));

    // Create images
    createImages(ctx, width, height);

    // Create descriptor layouts
    // Threshold: binding 0 = HDR sampler, binding 1 = UBO
    m_threshLayout = VkDescLayoutBuilder()
        .addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
        .addBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT)
        .build(device);

    // Blur: binding 0 = source sampler (push constant for direction)
    m_blurLayout = VkDescLayoutBuilder()
        .addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
        .build(device);

    // Tonemap: binding 0 = HDR sampler, binding 1 = bloom sampler, binding 2 = UBO
    m_tonemapLayout = VkDescLayoutBuilder()
        .addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
        .addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
        .addBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT)
        .build(device);

    // Pipeline layouts
    {
        VkPipelineLayoutCreateInfo ci{};
        ci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount = 1;
        ci.pSetLayouts    = &m_threshLayout;
        vkCreatePipelineLayout(device, &ci, nullptr, &m_threshPipeLayout);
    }
    {
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.size       = sizeof(glm::vec2); // blur direction

        VkPipelineLayoutCreateInfo ci{};
        ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount         = 1;
        ci.pSetLayouts            = &m_blurLayout;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges    = &pcRange;
        vkCreatePipelineLayout(device, &ci, nullptr, &m_blurPipeLayout);
    }
    {
        VkPipelineLayoutCreateInfo ci{};
        ci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount = 1;
        ci.pSetLayouts    = &m_tonemapLayout;
        vkCreatePipelineLayout(device, &ci, nullptr, &m_tonemapPipeLayout);
    }

    // Descriptor pool & sets
    createDescriptors(device);
    writeDescriptors(device);

    // Pipelines
    createPipelines(device);

    printf("[StratumV] PostProcess initialized (%ux%u, bloom %ux%u)\n",
        width, height, width / 2, height / 2);
    return true;
}

// ── Shutdown ────────────────────────────────────────────────────
void PostProcess::shutdown(VkDevice device, VmaAllocator alloc)
{
    destroyPipelines(device);

    if (m_threshPipeLayout)  vkDestroyPipelineLayout(device, m_threshPipeLayout, nullptr);
    if (m_blurPipeLayout)    vkDestroyPipelineLayout(device, m_blurPipeLayout, nullptr);
    if (m_tonemapPipeLayout) vkDestroyPipelineLayout(device, m_tonemapPipeLayout, nullptr);
    m_threshPipeLayout = m_blurPipeLayout = m_tonemapPipeLayout = VK_NULL_HANDLE;

    if (m_threshLayout)  vkDestroyDescriptorSetLayout(device, m_threshLayout, nullptr);
    if (m_blurLayout)    vkDestroyDescriptorSetLayout(device, m_blurLayout, nullptr);
    if (m_tonemapLayout) vkDestroyDescriptorSetLayout(device, m_tonemapLayout, nullptr);
    m_threshLayout = m_blurLayout = m_tonemapLayout = VK_NULL_HANDLE;

    m_descPool.destroy(device);

    destroyImages(device, alloc);
    m_ubo.destroy(alloc);

    m_ppVert.destroy(device);
    m_threshFrag.destroy(device);
    m_blurFrag.destroy(device);
    m_tonemapFrag.destroy(device);
}

// ── Resize ──────────────────────────────────────────────────────
void PostProcess::resize(VkCtx& ctx, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;

    VkDevice device = ctx.device();

    destroyImages(device, ctx.allocator());
    createImages(ctx, width, height);

    // Rewrite descriptors (image views changed)
    writeDescriptors(device);

    printf("[StratumV] PostProcess resized (%ux%u)\n", width, height);
}

// ── Record Commands ─────────────────────────────────────────────
void PostProcess::recordCommands(VkCommandBuffer cmd,
    VkImage swapImage, VkImageView swapImageView, VkExtent2D swapExtent)
{
    uint32_t halfW = m_width / 2;
    uint32_t halfH = m_height / 2;

    // ── 1. Threshold pass (full-res HDR → half-res bright) ──────
    transitionImage(cmd, m_brightPass.image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    {
        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView   = m_brightPass.view;
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = { halfW, halfH };
        ri.layerCount           = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments    = &colorAtt;

        vkCmdBeginRendering(cmd, &ri);

        VkViewport vp{};
        vp.width    = (float)halfW;
        vp.height   = (float)halfH;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D sc{};
        sc.extent = { halfW, halfH };
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_threshPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_threshPipeLayout, 0, 1, &m_threshDesc, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRendering(cmd);
    }

    transitionImage(cmd, m_brightPass.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // ── 2. Blur passes (ping-pong) ─────────────────────────────
    // First horizontal pass reads from brightPass (m_blurDesc uses brightPass for blur[0]'s source? No.)
    // We need: blur pass 0 H reads brightPass, writes blur[0]
    //          blur pass 0 V reads blur[0], writes blur[1]
    //          blur pass 1 H reads blur[1], writes blur[0]
    //          blur pass 1 V reads blur[0], writes blur[1]
    //          ...
    // But our descriptor sets are:
    //   m_blurDesc[0] reads brightPass (for first H pass) or blur[1] (for subsequent H passes)
    //   m_blurDesc[1] reads blur[0] (for V passes)
    // Actually we need to be smarter. Let's use:
    //   m_blurDesc[0] → reads brightPass
    //   m_blurDesc[1] → reads blur[0]
    // And for passes after first, we need a third desc that reads blur[1].
    // BUT we only have 2 blur desc sets allocated.
    //
    // Simpler approach: 3 blur desc sets:
    //   blurDesc[0] reads brightPass → writes blur[0] (first H)
    //   blurDesc[1] reads blur[0]    → writes blur[1] (all V)
    //   blurDesc[2] reads blur[1]    → writes blur[0] (subsequent H)
    //
    // Wait, let me reconsider. We have 2 blur ping-pong images.
    // For the very first horizontal pass, the source is brightPass.
    // For all subsequent passes, source alternates between blur[0] and blur[1].
    //
    // We need 3 descriptor sets for blur:
    //   - one that samples brightPass
    //   - one that samples blur[0]
    //   - one that samples blur[1]
    //
    // Let me just use the existing descriptor structure and handle it.
    // m_blurDesc[0] = reads blur[0], m_blurDesc[1] = reads blur[1]
    // For the first H pass (reading brightPass), we use m_threshDesc? No, wrong layout.
    // We need a separate desc set for brightPass → blur read.
    //
    // Actually the simplest fix: allocate a 3rd blur desc set for brightPass.
    // But let me just write brightPass into blur[1] first via a copy, then
    // start the ping-pong from blur[1].
    //
    // Even simpler: just use 3 blur descriptor sets. Let me restructure.
    // For now, I'll do the straightforward approach with 3 sets in the pool.

    // We have m_blurDesc[0] reads blur[0], m_blurDesc[1] reads blur[1]
    // But first H pass needs to read brightPass. We can use a 3rd desc for that.
    // Actually, I realize I should fix the descriptor allocation.
    // Let me use m_brightBlurDesc (reads brightPass), m_blurDesc[0] (reads blur[0]),
    // m_blurDesc[1] (reads blur[1]).

    // The current implementation has this handled via the descriptor setup in createDescriptors.
    // m_blurDesc[0] reads brightPass (for first H pass)
    // m_blurDesc[1] reads blur[0]   (for first V pass and subsequent)
    // We need a 3rd: m_blurDesc[2] reads blur[1] (for subsequent H passes)
    // ... but we only declared [2]. Let me just handle the ping-pong properly.

    // REVISED: The blur loop ping-pong:
    // Pass 0: H: read brightPass → blur[0], V: read blur[0] → blur[1]
    // Pass 1: H: read blur[1] → blur[0], V: read blur[0] → blur[1]
    // ...
    // So we need desc sets that read: brightPass, blur[0], blur[1]
    // That's what m_blurDesc[0..2] should be. Let me fix header later, for now
    // the code uses indices.

    // IMPORTANT: we actually have 3 blur desc sets, see createDescriptors.

    glm::vec2 hDir = glm::vec2(1.0f / (float)halfW, 0.0f);
    glm::vec2 vDir = glm::vec2(0.0f, 1.0f / (float)halfH);

    for (int pass = 0; pass < BLOOM_PASSES; pass++) {
        // Horizontal: read source → write blur[0]
        {
            // Source is brightPass (pass 0) or blur[1] (pass > 0)
            VkDescriptorSet srcDesc = (pass == 0) ? m_blurDesc[0] : m_blurDesc[2];

            transitionImage(cmd, m_blur[0].image,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

            VkRenderingAttachmentInfo colorAtt{};
            colorAtt.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAtt.imageView   = m_blur[0].view;
            colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo ri{};
            ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea.extent    = { halfW, halfH };
            ri.layerCount           = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments    = &colorAtt;

            vkCmdBeginRendering(cmd, &ri);

            VkViewport vp{};
            vp.width    = (float)halfW;
            vp.height   = (float)halfH;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);

            VkRect2D sc{};
            sc.extent = { halfW, halfH };
            vkCmdSetScissor(cmd, 0, 1, &sc);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_blurPipeLayout, 0, 1, &srcDesc, 0, nullptr);
            vkCmdPushConstants(cmd, m_blurPipeLayout,
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec2), &hDir);
            vkCmdDraw(cmd, 3, 1, 0, 0);

            vkCmdEndRendering(cmd);

            transitionImage(cmd, m_blur[0].image,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }

        // Vertical: read blur[0] → write blur[1]
        {
            transitionImage(cmd, m_blur[1].image,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

            VkRenderingAttachmentInfo colorAtt{};
            colorAtt.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAtt.imageView   = m_blur[1].view;
            colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo ri{};
            ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea.extent    = { halfW, halfH };
            ri.layerCount           = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments    = &colorAtt;

            vkCmdBeginRendering(cmd, &ri);

            VkViewport vp{};
            vp.width    = (float)halfW;
            vp.height   = (float)halfH;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);

            VkRect2D sc{};
            sc.extent = { halfW, halfH };
            vkCmdSetScissor(cmd, 0, 1, &sc);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_blurPipeLayout, 0, 1, &m_blurDesc[1], 0, nullptr);
            vkCmdPushConstants(cmd, m_blurPipeLayout,
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec2), &vDir);
            vkCmdDraw(cmd, 3, 1, 0, 0);

            vkCmdEndRendering(cmd);

            transitionImage(cmd, m_blur[1].image,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
    }

    // ── 3. Tonemap pass (HDR + bloom → swapchain) ───────────────
    transitionImage(cmd, swapImage,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    {
        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView   = swapImageView;
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = swapExtent;
        ri.layerCount           = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments    = &colorAtt;

        vkCmdBeginRendering(cmd, &ri);

        VkViewport vp{};
        vp.width    = (float)swapExtent.width;
        vp.height   = (float)swapExtent.height;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D sc{};
        sc.extent = swapExtent;
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_tonemapPipeLayout, 0, 1, &m_tonemapDesc, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRendering(cmd);
    }
}

// ── Update UBO ──────────────────────────────────────────────────
void PostProcess::updateParams(const PostProcessUBO& params)
{
    memcpy(m_ubo.info.pMappedData, &params, sizeof(PostProcessUBO));
}

// ── Hot-reload ──────────────────────────────────────────────────
bool PostProcess::checkReload(VkDevice device)
{
    bool reloaded = false;
    reloaded |= m_ppVert.checkReload(device);
    reloaded |= m_threshFrag.checkReload(device);
    reloaded |= m_blurFrag.checkReload(device);
    reloaded |= m_tonemapFrag.checkReload(device);

    if (reloaded) {
        destroyPipelines(device);
        createPipelines(device);
        printf("[StratumV] PostProcess pipelines rebuilt (hot-reload)\n");
    }
    return reloaded;
}

// ── Create images ───────────────────────────────────────────────
void PostProcess::createImages(VkCtx& ctx, uint32_t w, uint32_t h)
{
    VkFormat hdr = VK_FORMAT_R16G16B16A16_SFLOAT;
    uint32_t halfW = w / 2;
    uint32_t halfH = h / 2;

    m_hdrColor   = ColorImage::create(ctx, w, h, hdr);
    m_brightPass = ColorImage::create(ctx, halfW, halfH, hdr);
    m_blur[0]    = ColorImage::create(ctx, halfW, halfH, hdr);
    m_blur[1]    = ColorImage::create(ctx, halfW, halfH, hdr);
}

void PostProcess::destroyImages(VkDevice device, VmaAllocator alloc)
{
    m_hdrColor.destroy(device, alloc);
    m_brightPass.destroy(device, alloc);
    m_blur[0].destroy(device, alloc);
    m_blur[1].destroy(device, alloc);
}

// ── Descriptors ─────────────────────────────────────────────────
void PostProcess::createDescriptors(VkDevice device)
{
    // Pool: 1 thresh + 3 blur + 1 tonemap = 5 sets
    // Samplers: 1 (thresh) + 3×1 (blur) + 2 (tonemap) = 6
    // UBOs: 1 (thresh) + 1 (tonemap) = 2
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         2 },
    };
    m_descPool.init(device, 5, poolSizes, 2);

    m_threshDesc  = m_descPool.allocate(device, m_threshLayout);
    m_blurDesc[0] = m_descPool.allocate(device, m_blurLayout);  // reads brightPass
    m_blurDesc[1] = m_descPool.allocate(device, m_blurLayout);  // reads blur[0]
    m_blurDesc[2] = m_descPool.allocate(device, m_blurLayout);  // reads blur[1]
    m_tonemapDesc = m_descPool.allocate(device, m_tonemapLayout);
}

void PostProcess::writeDescriptors(VkDevice device)
{
    // Helper to write a single image descriptor
    auto writeImg = [&](VkDescriptorSet set, uint32_t binding, VkImageView view, VkSampler sampler) {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfo.imageView   = view;
        imgInfo.sampler     = sampler;

        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = set;
        w.dstBinding      = binding;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    };

    auto writeUbo = [&](VkDescriptorSet set, uint32_t binding) {
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_ubo.buffer;
        bufInfo.range  = sizeof(PostProcessUBO);

        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = set;
        w.dstBinding      = binding;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo     = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    };

    // Threshold: binding 0 = HDR scene, binding 1 = UBO
    writeImg(m_threshDesc, 0, m_hdrColor.view, m_hdrColor.sampler);
    writeUbo(m_threshDesc, 1);

    // Blur[0]: reads brightPass
    writeImg(m_blurDesc[0], 0, m_brightPass.view, m_brightPass.sampler);

    // Blur[1]: reads blur[0]
    writeImg(m_blurDesc[1], 0, m_blur[0].view, m_blur[0].sampler);

    // Blur[2]: reads blur[1]
    writeImg(m_blurDesc[2], 0, m_blur[1].view, m_blur[1].sampler);

    // Tonemap: binding 0 = HDR scene, binding 1 = bloom (blur[1], final result), binding 2 = UBO
    writeImg(m_tonemapDesc, 0, m_hdrColor.view, m_hdrColor.sampler);
    writeImg(m_tonemapDesc, 1, m_blur[1].view, m_blur[1].sampler);
    writeUbo(m_tonemapDesc, 2);
}

// ── Pipelines ───────────────────────────────────────────────────
void PostProcess::createPipelines(VkDevice device)
{
    VkFormat hdr = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Threshold
    m_threshPipeline = VkPipeBuilder()
        .setShaders(m_ppVert.module(), m_threshFrag.module())
        .setCullMode(VK_CULL_MODE_NONE)
        .setDepthTest(false, false)
        .setBlendEnabled(false)
        .setColorFormat(hdr)
        .setDepthFormat(VK_FORMAT_UNDEFINED)
        .setLayout(m_threshPipeLayout)
        .build(device);

    // Blur
    m_blurPipeline = VkPipeBuilder()
        .setShaders(m_ppVert.module(), m_blurFrag.module())
        .setCullMode(VK_CULL_MODE_NONE)
        .setDepthTest(false, false)
        .setBlendEnabled(false)
        .setColorFormat(hdr)
        .setDepthFormat(VK_FORMAT_UNDEFINED)
        .setLayout(m_blurPipeLayout)
        .build(device);

    // Tonemap
    m_tonemapPipeline = VkPipeBuilder()
        .setShaders(m_ppVert.module(), m_tonemapFrag.module())
        .setCullMode(VK_CULL_MODE_NONE)
        .setDepthTest(false, false)
        .setBlendEnabled(false)
        .setColorFormat(m_swapFormat)
        .setDepthFormat(VK_FORMAT_UNDEFINED)
        .setLayout(m_tonemapPipeLayout)
        .build(device);
}

void PostProcess::destroyPipelines(VkDevice device)
{
    if (m_threshPipeline)  vkDestroyPipeline(device, m_threshPipeline, nullptr);
    if (m_blurPipeline)    vkDestroyPipeline(device, m_blurPipeline, nullptr);
    if (m_tonemapPipeline) vkDestroyPipeline(device, m_tonemapPipeline, nullptr);
    m_threshPipeline = m_blurPipeline = m_tonemapPipeline = VK_NULL_HANDLE;
}

void PostProcess::setHdrSource(VkDevice device, VkImageView view, VkSampler sampler)
{
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView   = view;
    imgInfo.sampler     = sampler;

    VkWriteDescriptorSet writes[2]{};
    // Threshold binding 0
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_threshDesc;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo      = &imgInfo;
    // Tonemap binding 0
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_tonemapDesc;
    writes[1].dstBinding      = 0;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
}

void PostProcess::resetHdrSource(VkDevice device)
{
    setHdrSource(device, m_hdrColor.view, m_hdrColor.sampler);
}

} // namespace sv
