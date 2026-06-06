// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <cstdint>

namespace sv {

// Sort order constants for built-in render graph passes.
// Gaps of 10 allow DLL plugins to insert custom passes between them.
namespace PassOrder {
    constexpr uint32_t Shadow              = 0;
    constexpr uint32_t HZBBuild            = 7;
    constexpr uint32_t GPUCull             = 8;
    constexpr uint32_t DllCompute          = 10;
    constexpr uint32_t OceanFFT            = 20;
    constexpr uint32_t RTShadow            = 30;
    constexpr uint32_t ReSTIR              = 40;
    constexpr uint32_t SHaRC               = 50;
    constexpr uint32_t DepthRestore        = 55;
    constexpr uint32_t MainPass            = 60;
    constexpr uint32_t DLSS                = 70;
    constexpr uint32_t PostProcess         = 80;
    constexpr uint32_t UnderwaterDepthPre  = 85;
    constexpr uint32_t UIPass              = 90;
    constexpr uint32_t ImGuiOverlay        = 92;
    constexpr uint32_t UnderwaterDepthPost = 95;
    constexpr uint32_t PostUIPass          = 100;
    constexpr uint32_t Present             = 110;
}

// Pass name constants (used for setPassEnabled lookups)
namespace PassName {
    constexpr const char* Shadow              = "Shadow";
    constexpr const char* HZBBuild            = "HZBBuild";
    constexpr const char* GPUCull             = "GPUCull";
    constexpr const char* DllCompute          = "DllCompute";
    constexpr const char* OceanFFT            = "OceanFFT";
    constexpr const char* RTShadow            = "RTShadow";
    constexpr const char* ReSTIR              = "ReSTIR";
    constexpr const char* SHaRC               = "SHaRC";
    constexpr const char* DepthRestore        = "DepthRestore";
    constexpr const char* MainPass            = "MainPass";
    constexpr const char* DLSS                = "DLSS";
    constexpr const char* PostProcess         = "PostProc";
    constexpr const char* UnderwaterDepthPre  = "UW_DepthPre";
    constexpr const char* UIPass              = "UIPass";
    constexpr const char* ImGuiOverlay        = "ImGui";
    constexpr const char* UnderwaterDepthPost = "UW_DepthPost";
    constexpr const char* PostUIPass          = "PostUIPass";
    constexpr const char* Present             = "Present";
}

} // namespace sv
