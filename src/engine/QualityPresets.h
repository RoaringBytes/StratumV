// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <cstdint>

namespace sv {

enum class PresetLevel : int {
    Low    = 0,
    Medium = 1,
    High   = 2,
    Ultra  = 3,
    Custom = 4
};

struct QualityPreset {
    bool     rtShadows;        // true = use RT shadows (false = force rasterized)
    bool     restirEnabled;
    bool     sharcEnabled;
    bool     dlssEnabled;
    int      dlssQualityIndex; // 0=DLAA, 1=Quality, 2=Balanced, 3=Performance, 4=UltraPerf
    uint32_t shadowMapSize;    // 1024 / 2048 / 4096
    float    foliageDensity;   // blades per m^2
    int      foliageMaxBlades;
    float    foliageLodNear;
    float    foliageLodFar;
    bool     uiShadersEnabled; // false on Low to save GPU time
};

static constexpr int kPresetCount = 4;
static constexpr const char* kPresetNames[] = {
    "Low", "Medium", "High", "Ultra", "Custom"
};

//                              rtShad  restir  sharc   dlss    dlssQ  shadSz  fogDens  maxBld  lodN   lodF   uiShad
static constexpr QualityPreset kPresets[kPresetCount] = {
    /* Low    */ { false, false, false, true,  3,     1024,   5.0f,   100000, 25.0f, 45.0f, false },
    /* Medium */ { true,  false, false, true,  2,     2048,  10.0f,   200000, 30.0f, 55.0f, true  },
    /* High   */ { true,  true,  false, true,  1,     4096,  15.0f,   400000, 40.0f, 65.0f, true  },
    /* Ultra  */ { true,  true,  true,  true,  0,     4096,  25.0f,   600000, 50.0f, 80.0f, true  },
};

// Detect if current state matches any preset, return Custom if none match
inline PresetLevel detectPreset(bool rtShadows, bool restir, bool sharc,
                                bool dlssEnabled, int dlssQI, uint32_t shadowSz,
                                float fogDens, int maxBld, float lodN, float lodF,
                                bool uiShaders = true)
{
    for (int i = 0; i < kPresetCount; i++) {
        auto& p = kPresets[i];
        if (p.rtShadows == rtShadows && p.restirEnabled == restir &&
            p.sharcEnabled == sharc && p.dlssEnabled == dlssEnabled &&
            p.dlssQualityIndex == dlssQI && p.shadowMapSize == shadowSz &&
            p.foliageDensity == fogDens && p.foliageMaxBlades == maxBld &&
            p.foliageLodNear == lodN && p.foliageLodFar == lodF &&
            p.uiShadersEnabled == uiShaders)
            return (PresetLevel)i;
    }
    return PresetLevel::Custom;
}

} // namespace sv
