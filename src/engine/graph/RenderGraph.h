// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "GraphResources.h"
#include "GpuProfiler.h"
#include "../RenderPass.h"

#include <functional>
#include <vector>
#include <string>

namespace sv {

// DLL-safe pass recording callback (C function pointer, not std::function).
// Called by the graph executor for each enabled DLL pass.
using DllPassRecordFn = void(*)(VkCommandBuffer cmd, const FrameData* frame, const void* userData);

// One render/compute/RT pass in the graph
struct PassNode {
    const char*     name       = nullptr;
    uint32_t        sortOrder  = 0;

    // Recording callback — captures Engine* and calls the existing record*() method
    std::function<void(VkCommandBuffer cmd, const FrameData& frame)> recordFn;

    // Enable predicate — evaluated every frame. nullptr = always enabled.
    std::function<bool()> enableFn;

    // Resource declarations (graph uses these to insert barriers)
    std::vector<ResourceUsage> reads;
    std::vector<ResourceUsage> writes;

    // Runtime hot-toggle (independent of enableFn)
    bool        enabled      = true;

    // Assigned by GpuProfiler during compile
    int32_t     profilerSlot = -1;
};

// Data-driven render graph — linear pass executor with automatic barrier insertion.
// Replaces hardcoded Engine::recordFrame().
class RenderGraph {
public:
    // ── Build phase (called once at init, or on recompile) ──────

    ResourceId registerImage(const char* name, VkImage image,
                             VkImageLayout initialLayout,
                             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
    {
        return m_resources.registerImage(name, image, initialLayout, aspect);
    }

    ResourceId registerBuffer(const char* name, VkBuffer buffer)
    {
        return m_resources.registerBuffer(name, buffer);
    }

    // Add a pass. Returns reference for the caller to fill in reads/writes/recordFn.
    PassNode& addPass(const char* name, uint32_t sortOrder);

    // Sort passes by sortOrder, assign profiler slots. Call after all passes added.
    void compile(VkDevice device, float timestampPeriod);

    // ── Per-frame execution ─────────────────────────────────────

    // Update dynamic resources that change per-frame
    void updateImage(ResourceId id, VkImage newImage)
    {
        m_resources.updateImage(id, newImage);
    }

    // Execute all enabled passes: reset layouts, insert barriers, record, profile.
    void execute(VkCommandBuffer cmd, const FrameData& frame, uint32_t currentFrame);

    // ── Runtime modification ────────────────────────────────────

    void setPassEnabled(const char* name, bool enabled);
    bool isPassEnabled(const char* name) const;

    // ── DLL pass registration (Layer 8 plugins) ─────────────────

    // Register a DLL pass. Uses C function pointer for DLL safety.
    // The graph is re-sorted on next execute(). Returns true on success.
    bool registerDllPass(const char* name, uint32_t sortOrder,
                         DllPassRecordFn recordFn, const void* userData);

    // Unregister a DLL pass by name. Called during DLL hot-reload unload.
    void unregisterDllPass(const char* name);

    // ── Profiler access ─────────────────────────────────────────

    GpuProfiler&       profiler()       { return m_profiler; }
    const GpuProfiler& profiler() const { return m_profiler; }

    // ── Resource access ─────────────────────────────────────────

    ResourceRegistry&       resources()       { return m_resources; }
    const ResourceRegistry& resources() const { return m_resources; }

    void shutdown();

private:
    std::vector<PassNode>  m_passes;
    ResourceRegistry       m_resources;
    GpuProfiler            m_profiler;
    VkDevice               m_device       = VK_NULL_HANDLE;
    float                  m_tsPeriod     = 0.f;
    bool                   m_compiled     = false;
    bool                   m_needsResort  = false;
};

} // namespace sv
