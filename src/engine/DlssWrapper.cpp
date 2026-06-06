// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "DlssWrapper.h"
#include "vk/VkContext.h"
#include <cstdio>

#ifdef DLSS_AVAILABLE
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#endif

namespace sv {

#ifdef DLSS_AVAILABLE

static NVSDK_NGX_PerfQuality_Value toNgxQuality(DlssQuality q) {
    switch (q) {
        case DlssQuality::DLAA:             return NVSDK_NGX_PerfQuality_Value_DLAA;
        case DlssQuality::Quality:          return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case DlssQuality::Balanced:         return NVSDK_NGX_PerfQuality_Value_Balanced;
        case DlssQuality::Performance:      return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case DlssQuality::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        default:                            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

bool DlssWrapper::init(VkCtx& ctx, uint32_t outputW, uint32_t outputH, DlssQuality quality) {
    m_device  = ctx.device();
    m_outputW = outputW;
    m_outputH = outputH;

    // Set up feature search paths so NGX can find nvngx_dlss.dll
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';

    const wchar_t* featurePaths[] = { exePath };
    NVSDK_NGX_FeatureCommonInfo featureInfo = {};
    featureInfo.PathListInfo.Path = featurePaths;
    featureInfo.PathListInfo.Length = 1;

    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init(
        231313132,
        exePath,
        ctx.instance(), ctx.physicalDevice(), ctx.device(),
        vkGetInstanceProcAddr, vkGetDeviceProcAddr,
        &featureInfo);

    if (NVSDK_NGX_FAILED(result)) {
        printf("[StratumV] DLSS: NGX init failed (0x%08x)\n", result);
        return false;
    }

    // Get capability parameters
    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&m_params);
    if (NVSDK_NGX_FAILED(result)) {
        printf("[StratumV] DLSS: GetCapabilityParameters failed (0x%08x)\n", result);
        NVSDK_NGX_VULKAN_Shutdown1(m_device);
        return false;
    }

    // Check if DLSS-SR is supported
    int dlssAvailable = 0;
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSampling_Available, &dlssAvailable);
    if (!dlssAvailable) {
        printf("[StratumV] DLSS: Super Resolution not available on this GPU\n");
        NVSDK_NGX_VULKAN_Shutdown1(m_device);
        return false;
    }

    // Create the DLSS feature with optimal render resolution
    if (!setQuality(ctx, quality, outputW, outputH)) {
        NVSDK_NGX_VULKAN_Shutdown1(m_device);
        return false;
    }

    m_available = true;
    printf("[StratumV] DLSS: Initialized. Render %ux%u -> Output %ux%u\n",
        m_renderW, m_renderH, m_outputW, m_outputH);
    return true;
}

bool DlssWrapper::setQuality(VkCtx& ctx, DlssQuality q, uint32_t outW, uint32_t outH) {
    if (!m_params) return false;

    if (m_handle) {
        NVSDK_NGX_VULKAN_ReleaseFeature(m_handle);
        m_handle = nullptr;
    }

    m_outputW = outW;
    m_outputH = outH;

    auto ngxQuality = toNgxQuality(q);

    unsigned int optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
    float sharpness = 0.0f;
    NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(m_params,
        outW, outH, ngxQuality,
        &optW, &optH, &maxW, &maxH, &minW, &minH, &sharpness);

    if (NVSDK_NGX_FAILED(result) || optW == 0 || optH == 0) {
        printf("[StratumV] DLSS: GetOptimalSettings failed (0x%08x)\n", result);
        return false;
    }

    m_renderW = optW;
    m_renderH = optH;

    NVSDK_NGX_DLSS_Create_Params createParams = {};
    createParams.Feature.InWidth  = m_renderW;
    createParams.Feature.InHeight = m_renderH;
    createParams.Feature.InTargetWidth  = m_outputW;
    createParams.Feature.InTargetHeight = m_outputH;
    createParams.Feature.InPerfQualityValue = ngxQuality;
    createParams.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

    VkCommandBuffer cmd = ctx.beginSingleTimeCommands();
    result = NGX_VULKAN_CREATE_DLSS_EXT1(
        m_device, cmd, 1, 1, &m_handle, m_params, &createParams);
    ctx.endSingleTimeCommands(cmd);

    if (NVSDK_NGX_FAILED(result)) {
        printf("[StratumV] DLSS: CreateFeature failed (0x%08x)\n", result);
        m_handle = nullptr;
        return false;
    }

    printf("[StratumV] DLSS: Feature created. Render %ux%u -> %ux%u\n",
        m_renderW, m_renderH, m_outputW, m_outputH);
    return true;
}

void DlssWrapper::evaluate(VkCommandBuffer cmd,
    VkImage colorIn, VkImageView colorView, VkFormat colorFormat,
    VkImage depthIn, VkImageView depthView,
    VkImage mvIn, VkImageView mvView, VkFormat mvFormat,
    VkImage colorOut, VkImageView colorOutView, VkFormat colorOutFormat,
    uint32_t renderW, uint32_t renderH,
    uint32_t outputW, uint32_t outputH,
    float jitterX, float jitterY, bool reset)
{
    if (!m_handle || !m_params) return;

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;

    VkImageSubresourceRange depthRange{};
    depthRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthRange.levelCount = 1;
    depthRange.layerCount = 1;

    NVSDK_NGX_Resource_VK colorRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        colorView, colorIn, range, colorFormat, renderW, renderH, false);
    NVSDK_NGX_Resource_VK depthRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        depthView, depthIn, depthRange, VK_FORMAT_D32_SFLOAT, renderW, renderH, false);
    NVSDK_NGX_Resource_VK mvRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        mvView, mvIn, range, mvFormat, renderW, renderH, false);
    NVSDK_NGX_Resource_VK outputRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        colorOutView, colorOut, range, colorOutFormat, outputW, outputH, true);

    NVSDK_NGX_VK_DLSS_Eval_Params evalParams = {};
    evalParams.Feature.pInColor   = &colorRes;
    evalParams.Feature.pInOutput  = &outputRes;
    evalParams.pInDepth           = &depthRes;
    evalParams.pInMotionVectors   = &mvRes;
    evalParams.InJitterOffsetX    = jitterX;
    evalParams.InJitterOffsetY    = jitterY;
    evalParams.InReset            = reset ? 1 : 0;
    evalParams.InRenderSubrectDimensions.Width  = renderW;
    evalParams.InRenderSubrectDimensions.Height = renderH;
    // MV scale: shaders output (curNDC - prevNDC) * 0.5 in NDC space
    // DLSS expects pixel-space MVs. Scale converts NDC*0.5 to pixels.
    evalParams.InMVScaleX = -(float)renderW * 0.5f;
    evalParams.InMVScaleY =  (float)renderH * 0.5f;

    NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, m_handle, m_params, &evalParams);
}

void DlssWrapper::shutdown() {
    if (m_handle) {
        NVSDK_NGX_VULKAN_ReleaseFeature(m_handle);
        m_handle = nullptr;
    }
    if (m_device) {
        NVSDK_NGX_VULKAN_Shutdown1(m_device);
    }
    m_available = false;
    m_params = nullptr;
}

#else // !DLSS_AVAILABLE

bool DlssWrapper::init(VkCtx&, uint32_t, uint32_t, DlssQuality) {
    printf("[StratumV] DLSS: Not compiled (ENABLE_DLSS=OFF)\n");
    return false;
}
void DlssWrapper::shutdown() {}
void DlssWrapper::evaluate(VkCommandBuffer, VkImage, VkImageView, VkFormat,
    VkImage, VkImageView, VkImage, VkImageView, VkFormat,
    VkImage, VkImageView, VkFormat, uint32_t, uint32_t, uint32_t, uint32_t,
    float, float, bool) {}
bool DlssWrapper::setQuality(VkCtx&, DlssQuality, uint32_t, uint32_t) { return false; }

#endif

} // namespace sv
