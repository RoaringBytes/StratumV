// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <cstdint>
#include <vector>
#include <cstring>

namespace sv {

// Per-pass GPU timestamp profiler.
// Owns a single VkQueryPool, assigns slot indices at compile time,
// reads results after fence signal with VK_QUERY_RESULT_WAIT_BIT.
class GpuProfiler {
public:
    static constexpr uint32_t MAX_FRAMES = 2; // MAX_FRAMES_IN_FLIGHT

    void init(VkDevice device, float timestampPeriod, uint32_t maxPasses)
    {
        m_device  = device;
        m_period  = timestampPeriod;
        m_maxSlots = maxPasses;

        // Pool: 2 timestamps per pass (begin + end) * frames in flight
        VkQueryPoolCreateInfo ci{};
        ci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        ci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = maxPasses * 2 * MAX_FRAMES;
        vkCreateQueryPool(device, &ci, nullptr, &m_pool);
    }

    void shutdown()
    {
        if (m_pool && m_device) {
            vkDestroyQueryPool(m_device, m_pool, nullptr);
            m_pool = VK_NULL_HANDLE;
        }
    }

    // Assign a named slot during graph compile. Returns slot index.
    int32_t assignSlot(const char* name)
    {
        int32_t idx = static_cast<int32_t>(m_slots.size());
        m_slots.push_back({name, 0.f});
        return idx;
    }

    // Reset query range for this frame (call at start of command buffer)
    void beginFrame(VkCommandBuffer cmd, uint32_t currentFrame)
    {
        if (!m_enabled || !m_pool) return;
        uint32_t base = currentFrame * m_maxSlots * 2;
        vkCmdResetQueryPool(cmd, m_pool, base, m_maxSlots * 2);
    }

    // Write timestamp before/after a pass
    void writeTimestamp(VkCommandBuffer cmd, int32_t slot, bool isBegin, uint32_t currentFrame)
    {
        if (!m_enabled || !m_pool || slot < 0) return;
        uint32_t base = currentFrame * m_maxSlots * 2;
        uint32_t queryIdx = base + static_cast<uint32_t>(slot) * 2 + (isBegin ? 0 : 1);
        VkPipelineStageFlagBits stage = isBegin
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        vkCmdWriteTimestamp(cmd, stage, m_pool, queryIdx);
    }

    // Read completed frame's timestamps. Call AFTER vkWaitForFences.
    void readback(uint32_t currentFrame)
    {
        if (!m_enabled || !m_pool || m_slots.empty()) return;

        uint32_t count = static_cast<uint32_t>(m_slots.size()) * 2;
        uint32_t base  = currentFrame * m_maxSlots * 2;

        // Size for actual slots used (not maxSlots)
        std::vector<uint64_t> ts(count);
        // No WAIT_BIT: fence already guarantees GPU completion.
        // If queries were never written (profiling just enabled), returns VK_NOT_READY.
        VkResult r = vkGetQueryPoolResults(
            m_device, m_pool, base, count,
            count * sizeof(uint64_t), ts.data(), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT);

        if (r != VK_SUCCESS) return;

        constexpr float EMA_ALPHA = 0.1f;
        for (size_t i = 0; i < m_slots.size(); i++) {
            uint64_t begin = ts[i * 2];
            uint64_t end   = ts[i * 2 + 1];
            float ms = static_cast<float>(end - begin) * m_period / 1e6f;
            // EMA smoothing
            m_slots[i].timeMs = m_slots[i].timeMs * (1.f - EMA_ALPHA) + ms * EMA_ALPHA;
        }
    }

    // Accessors
    float passTimeMs(int32_t slot) const
    {
        if (slot < 0 || slot >= static_cast<int32_t>(m_slots.size())) return 0.f;
        return m_slots[slot].timeMs;
    }

    int32_t slotCount() const { return static_cast<int32_t>(m_slots.size()); }

    const char* slotName(int32_t slot) const
    {
        if (slot < 0 || slot >= static_cast<int32_t>(m_slots.size())) return "";
        return m_slots[slot].name;
    }

    void setEnabled(bool e) { m_enabled = e; }
    bool isEnabled() const  { return m_enabled; }

    // Direct access to pass times array for DevServer/admin HUD
    void fillPassTimes(float* out, int maxCount) const
    {
        int n = static_cast<int>(m_slots.size());
        if (n > maxCount) n = maxCount;
        for (int i = 0; i < n; i++)
            out[i] = m_slots[i].timeMs;
        for (int i = n; i < maxCount; i++)
            out[i] = 0.f;
    }

    void clearSlots() { m_slots.clear(); }

private:
    struct Slot {
        const char* name;
        float       timeMs;
    };

    VkDevice     m_device  = VK_NULL_HANDLE;
    VkQueryPool  m_pool    = VK_NULL_HANDLE;
    float        m_period  = 0.f;
    uint32_t     m_maxSlots = 0;
    bool         m_enabled = false;
    std::vector<Slot> m_slots;
};

} // namespace sv
