// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <cstdint>

#ifdef DLSS_AVAILABLE
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;
#endif

namespace sv {

class VkCtx;

enum class DlssQuality {
    Off = -1,
    DLAA = 0,         // Native resolution (no upscaling, just AA)
    Quality,           // ~67% render resolution
    Balanced,          // ~58% render resolution
    Performance,       // ~50% render resolution
    UltraPerformance   // ~33% render resolution
};

class DlssWrapper {
public:
    bool init(VkCtx& ctx, uint32_t outputW, uint32_t outputH, DlssQuality quality);
    void shutdown();

    void evaluate(VkCommandBuffer cmd,
        VkImage colorIn, VkImageView colorView, VkFormat colorFormat,
        VkImage depthIn, VkImageView depthView,
        VkImage mvIn, VkImageView mvView, VkFormat mvFormat,
        VkImage colorOut, VkImageView colorOutView, VkFormat colorOutFormat,
        uint32_t renderW, uint32_t renderH,
        uint32_t outputW, uint32_t outputH,
        float jitterX, float jitterY, bool reset);

    bool setQuality(VkCtx& ctx, DlssQuality q, uint32_t outW, uint32_t outH);

    uint32_t renderWidth()  const { return m_renderW; }
    uint32_t renderHeight() const { return m_renderH; }
    bool     isAvailable()  const { return m_available; }

private:
    bool     m_available = false;
    uint32_t m_renderW = 0;
    uint32_t m_renderH = 0;
    uint32_t m_outputW = 0;
    uint32_t m_outputH = 0;
    VkDevice m_device = VK_NULL_HANDLE;

#ifdef DLSS_AVAILABLE
    NVSDK_NGX_Parameter* m_params = nullptr;
    NVSDK_NGX_Handle*    m_handle = nullptr;
#endif
};

} // namespace sv
