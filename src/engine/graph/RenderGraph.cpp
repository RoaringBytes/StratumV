// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "RenderGraph.h"
#include <algorithm>
#include <cassert>

namespace sv {

PassNode& RenderGraph::addPass(const char* name, uint32_t sortOrder)
{
    m_passes.push_back({});
    auto& p = m_passes.back();
    p.name      = name;
    p.sortOrder = sortOrder;
    return p;
}

void RenderGraph::compile(VkDevice device, float timestampPeriod)
{
    m_device   = device;
    m_tsPeriod = timestampPeriod;

    // Sort passes by explicit sort order
    std::sort(m_passes.begin(), m_passes.end(),
        [](const PassNode& a, const PassNode& b) { return a.sortOrder < b.sortOrder; });

    // Assign profiler slots
    uint32_t maxSlots = static_cast<uint32_t>(m_passes.size());
    m_profiler.clearSlots();
    m_profiler.init(device, timestampPeriod, maxSlots);
    for (auto& pass : m_passes)
        pass.profilerSlot = m_profiler.assignSlot(pass.name);

    m_compiled     = true;
    m_needsResort  = false;
}

void RenderGraph::execute(VkCommandBuffer cmd, const FrameData& frame, uint32_t currentFrame)
{
    assert(m_compiled);

    // Re-sort and recompile profiler if DLL passes were added/removed
    if (m_needsResort) {
        compile(m_device, m_tsPeriod);
    }

    // Reset all resource layouts to their initial state
    m_resources.resetLayouts();

    // Reset profiler queries for this frame
    m_profiler.beginFrame(cmd, currentFrame);

    // Scratch space for barriers (avoid per-pass allocation)
    std::vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(8);

    for (auto& pass : m_passes) {
        // Check both runtime toggle and per-frame enable predicate
        bool passActive = pass.enabled && (!pass.enableFn || pass.enableFn());

        if (!passActive) {
            // Write back-to-back timestamps for disabled passes (zero duration).
            // Required: readback reads ALL slots, so every query must be written.
            m_profiler.writeTimestamp(cmd, pass.profilerSlot, true, currentFrame);
            m_profiler.writeTimestamp(cmd, pass.profilerSlot, false, currentFrame);
            continue;
        }

        // Gather barriers needed for this pass
        barriers.clear();
        VkPipelineStageFlags srcStageMask = 0;
        VkPipelineStageFlags dstStageMask = 0;

        auto processUsage = [&](const ResourceUsage& usage) {
            if (!usage.resource.valid()) return;
            auto& res = m_resources.get(usage.resource);
            if (!res.isImage) return; // buffer barriers not needed for current pipeline

            VkImageLayout oldLayout = res.currentLayout;
            VkImageLayout newLayout = usage.requiredLayout;

            if (oldLayout == newLayout) return;

            VkImageMemoryBarrier barrier{};
            barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout           = oldLayout;
            barrier.newLayout           = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image               = res.image;
            barrier.subresourceRange.aspectMask     = usage.aspectMask;
            barrier.subresourceRange.baseMipLevel   = 0;
            barrier.subresourceRange.levelCount     = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount     = usage.layerCount;

            // Infer src access from old layout
            switch (oldLayout) {
                case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    break;
                case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    break;
                case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    break;
                case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    break;
                case VK_IMAGE_LAYOUT_GENERAL:
                    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    break;
                case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    break;
                default:
                    barrier.srcAccessMask = 0;
                    break;
            }

            // Dst access from the usage declaration
            barrier.dstAccessMask = usage.accessMask;

            // Infer src stage from old layout
            VkPipelineStageFlags srcStage = 0;
            switch (oldLayout) {
                case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                    srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                    break;
                case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                    srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                    break;
                case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                    srcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                    break;
                case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                    srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                    break;
                case VK_IMAGE_LAYOUT_GENERAL:
                    srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                    break;
                case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                    srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                    break;
                default:
                    srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                    break;
            }

            srcStageMask |= srcStage;
            dstStageMask |= usage.stageMask;

            barriers.push_back(barrier);

            // Update tracked layout
            res.currentLayout = newLayout;
        };

        for (const auto& u : pass.reads)
            processUsage(u);
        for (const auto& u : pass.writes)
            processUsage(u);

        // Emit all barriers in a single call
        if (!barriers.empty()) {
            vkCmdPipelineBarrier(cmd,
                srcStageMask ? srcStageMask : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                dstStageMask ? dstStageMask : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0,
                0, nullptr,
                0, nullptr,
                static_cast<uint32_t>(barriers.size()), barriers.data());
        }

        // Profile + record
        m_profiler.writeTimestamp(cmd, pass.profilerSlot, true, currentFrame);
        pass.recordFn(cmd, frame);
        m_profiler.writeTimestamp(cmd, pass.profilerSlot, false, currentFrame);
    }
}

void RenderGraph::setPassEnabled(const char* name, bool enabled)
{
    for (auto& p : m_passes) {
        if (strcmp(p.name, name) == 0) {
            p.enabled = enabled;
            return;
        }
    }
}

bool RenderGraph::isPassEnabled(const char* name) const
{
    for (const auto& p : m_passes) {
        if (strcmp(p.name, name) == 0)
            return p.enabled;
    }
    return false;
}

bool RenderGraph::registerDllPass(const char* name, uint32_t sortOrder,
                                   DllPassRecordFn recordFn, const void* userData)
{
    if (!name || !recordFn) return false;

    // Reject duplicate names
    for (const auto& p : m_passes) {
        if (strcmp(p.name, name) == 0)
            return false;
    }

    PassNode p{};
    p.name      = name;
    p.sortOrder = sortOrder;
    // Wrap the C function pointer in a std::function that forwards FrameData by pointer
    p.recordFn  = [recordFn, userData](VkCommandBuffer cmd, const FrameData& frame) {
        recordFn(cmd, &frame, userData);
    };
    m_passes.push_back(std::move(p));
    m_needsResort = true;
    return true;
}

void RenderGraph::unregisterDllPass(const char* name)
{
    if (!name) return;
    for (auto it = m_passes.begin(); it != m_passes.end(); ++it) {
        if (strcmp(it->name, name) == 0) {
            m_passes.erase(it);
            m_needsResort = true;
            return;
        }
    }
}

void RenderGraph::shutdown()
{
    m_profiler.shutdown();
    m_passes.clear();
    m_compiled = false;
}

} // namespace sv
