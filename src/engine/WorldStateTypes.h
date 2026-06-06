// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <glm/glm.hpp>
#include <cstring>

namespace sv {

// ── Time-of-Day Anchor ──────────────────────────────────────────
// Snapshot of all scene parameters at one ToD point (~85 fields).
// Interpolated by WorldStateResolver to drive SceneUBO + Ocean.
struct TimeOfDayAnchor {
    // Solar position (degrees)
    float solarElevation = 75.0f;   // -90..90
    float solarAzimuth   = 180.0f;  // 0..360

    // Sun
    glm::vec3 sunColor      = {1.0f, 0.95f, 0.85f};
    float     sunIntensity  = 5.0f;
    glm::vec3 ambientColor  = {0.15f, 0.18f, 0.25f};

    // Sky gradient
    glm::vec3 skyZenith     = {0.08f, 0.18f, 0.55f};
    glm::vec3 skyHorizon    = {0.45f, 0.65f, 0.85f};
    glm::vec3 skyMid        = {0.20f, 0.40f, 0.75f};
    glm::vec3 nadirColor    = {0.02f, 0.04f, 0.08f};

    // Sun glow
    glm::vec3 wideGlowColor    = {1.0f, 0.7f, 0.3f};
    float     wideGlowStrength = 0.35f;
    float     sunGlowPower     = 32.0f;
    float     sunDiskSize      = 0.005f;
    float     sunGlowMultiplier = 0.6f;
    float     wideGlowExponent  = 8.0f;

    // Clouds - colors
    glm::vec3 cloudBrightColor = {1.0f, 0.98f, 0.95f};
    glm::vec3 cloudShadowColor = {0.35f, 0.40f, 0.50f};
    glm::vec3 cloudGlowColor   = {1.0f, 0.85f, 0.5f};

    // Clouds - shape
    float cloudDensity     = 0.5f;
    float cloudSpeed       = 1.0f;
    float cloudHeight1     = 800.0f;
    float cloudScale1      = 0.00018f;
    float cloudDetail1     = 2.2f;
    float cloudSoftness    = 0.24f;
    float cloudHeight2     = 1500.0f;
    float cloudScale2      = 0.00012f;
    float cloudDetail2     = 3.0f;
    float cloudOpacity2    = 0.4f;
    float cloudLayerBlend1 = 0.8f;
    float cloudLayerBlend2 = 0.5f;
    float cloudFadeStart   = 0.01f;
    float cloudFadeEnd     = 0.15f;
    float cloudSunEdgeExp  = 4.0f;
    float cloudMinLighting = 0.35f;

    // God rays
    glm::vec3 godRayColor     = {1.0f, 0.9f, 0.6f};
    float     godRayIntensity = 0.5f;
    float     godRayDensity   = 1.0f;
    float     godRayFalloff   = 3.5f;
    float     godRayMultiplier = 0.5f;
    float     godRaySpread    = 1.0f;
    float     godRayLength    = 1.0f;
    float     godRayWidth     = 1.0f;
    float     godRayCount     = 1.0f;
    float     godRayAsymmetry = 0.0f;

    // Atmosphere
    glm::vec3 sunsetWarmColor   = {1.0f, 0.45f, 0.15f};
    glm::vec3 sunsetPinkColor   = {0.9f, 0.4f, 0.6f};
    glm::vec3 hazeColorDay      = {0.65f, 0.78f, 0.90f};
    glm::vec3 hazeColorSunset   = {1.0f, 0.6f, 0.3f};
    float     skyHaze           = 0.35f;
    float     sunsetStartHeight = 0.3f;
    float     sunsetWarmStrength = 0.65f;
    float     sunsetPinkStrength = 0.25f;
    float     nightDarkness     = 0.08f;
    float     hazeFalloff       = 5.0f;
    float     saturationBoost   = 1.2f;
    float     skyGradientExponent = 0.45f;
    float     skyGradientSplit    = 0.4f;

    // Fog
    glm::vec3 fogColor   = {0.7f, 0.75f, 0.82f};
    float     fogDensity = 0.0f;
    float     fogStart   = 50.0f;
    float     fogEnd     = 500.0f;

    // Water - colors (render-side only, no spectrum params)
    glm::vec3 scatterColor    = {0.0f, 0.80f, 0.50f};
    glm::vec3 deepColor       = {0.01f, 0.15f, 0.35f};
    glm::vec3 sssColor        = {0.0f, 0.65f, 0.50f};
    glm::vec3 skyReflectLow   = {0.15f, 0.35f, 0.60f};
    glm::vec3 skyReflectHigh  = {0.08f, 0.25f, 0.65f};
    glm::vec3 sunReflectSharp = {1.0f, 0.95f, 0.85f};
    glm::vec3 sunReflectBroad = {1.0f, 0.90f, 0.70f};
    glm::vec3 hazeColor_water = {0.55f, 0.70f, 0.88f};
    glm::vec3 foamTint        = {0.85f, 1.0f, 0.98f};

    // Water - material
    float sssStrength        = 0.65f;
    float refractionStrength = 0.02f;
    float causticIntensity   = 0.40f;
    float causticDepth       = 6.0f;
    float sparkleIntensity   = 10.0f;
    float sparklePower       = 1200.0f;
    float hazeStrength       = 0.00012f;

    // Shadow
    float shadowIntensity = 1.0f;

    // Night Sky
    float moonElevation     = -45.0f;   // degrees, -90..90
    float moonAzimuth       = 90.0f;    // degrees, 0..360
    float moonPhase         = 0.0f;     // 0=new, 0.25=Q1, 0.5=full, 0.75=Q3
    float starBrightness    = 0.0f;     // 0..1
    float starDensity       = 400.0f;   // grid cells per radian
    float milkyWayStrength  = 0.0f;     // 0..1
    float starTwinkleAmount = 0.3f;     // 0..1
};

// ── Sparse Property Override ────────────────────────────────────
// If enabled, the value overrides the ToD-interpolated base.
template<typename T>
struct WorldStateProperty {
    bool enabled = false;
    T    value{};
};

// ── Weather Override ────────────────────────────────────────────
// Sparse overrides layered on top of ToD base. Only enabled
// properties take effect; everything else passes through.
struct WeatherOverride {
    // Clouds (shape)
    WorldStateProperty<float> cloudDensity;
    WorldStateProperty<float> cloudSpeed;
    WorldStateProperty<float> cloudSoftness;
    WorldStateProperty<float> cloudOpacity2;
    WorldStateProperty<float> cloudLayerBlend1;
    WorldStateProperty<float> cloudLayerBlend2;
    WorldStateProperty<float> cloudMinLighting;

    // Ocean dynamics (write to Ocean directly, not TimeOfDayAnchor)
    WorldStateProperty<float> windSpeed;
    WorldStateProperty<float> amplitude;
    WorldStateProperty<float> choppiness;
    WorldStateProperty<float> dampingScale;
    WorldStateProperty<float> peakEnhancement;
    WorldStateProperty<float> swellStrength;
    WorldStateProperty<float> foamGenerationStrength;

    // Fog
    WorldStateProperty<float> fogDensity;
    WorldStateProperty<float> fogStart;
    WorldStateProperty<float> fogEnd;

    // Atmosphere
    WorldStateProperty<float> skyHaze;
    WorldStateProperty<float> saturationBoost;

    // God rays
    WorldStateProperty<float> godRayIntensity;

    float transitionTime = 5.0f;  // seconds for blend-in/out
};

// ── Weather State (runtime) ─────────────────────────────────────
// Tracks current weather target + transition progress.
// Uses char[64] for presetName to avoid std::string across DLL boundary.
struct WeatherState {
    WeatherOverride current;         // target override
    WeatherOverride snapshot;        // values at transition start
    float           blendProgress = 1.0f;  // 0 = snapshot, 1 = fully current
    bool            active        = false;
    int             presetIndex   = -1;
    char            presetName[64] = {};

    void setName(const char* n) {
        std::strncpy(presetName, n ? n : "", sizeof(presetName) - 1);
        presetName[sizeof(presetName) - 1] = '\0';
    }
};

// ── Biome Override (stub) ───────────────────────────────────────
// Sparse overrides for biome-specific scene adjustments.
// Currently only waterColorTint has a rendering effect.
struct BiomeOverride {
    WorldStateProperty<float>     temperature;        // celsius-ish (stored, no render effect yet)
    WorldStateProperty<float>     humidity;            // 0..1 (stored, no render effect yet)
    WorldStateProperty<float>     vegetationDensity;   // 0..1 (stored, no render effect yet)
    WorldStateProperty<glm::vec3> waterColorTint;      // multiplicative tint on scatter/deep colors
    float transitionTime = 5.0f;
};

// ── Biome State (runtime) ──────────────────────────────────────
struct BiomeState {
    BiomeOverride current;
    BiomeOverride snapshot;
    float         blendProgress = 1.0f;
    bool          active        = false;
    int           presetIndex   = -1;
    char          presetName[64] = {};

    void setName(const char* n) {
        std::strncpy(presetName, n ? n : "", sizeof(presetName) - 1);
        presetName[sizeof(presetName) - 1] = '\0';
    }
};

// ── World Clock ─────────────────────────────────────────────────
struct WorldClock {
    float timeOfDay = 12.0f;  // 0.0 - 24.0
    float speed     = 1.0f;   // game-hours per real-minute
    bool  playing   = false;
    bool  wseEnabled = false;
    int   selectedAnchorIdx = 2;  // default Noon (shared with AdminPanel + WorldStateSystem)
};

// ── Anchor indices ──────────────────────────────────────────────
enum AnchorIndex : int {
    Anchor_Dawn    = 0,
    Anchor_Morning = 1,
    Anchor_Noon    = 2,
    Anchor_Sunset  = 3,
    Anchor_Dusk    = 4,
    Anchor_Night   = 5
};

static constexpr int   ANCHOR_COUNT = 6;
static constexpr float ANCHOR_TIMES[ANCHOR_COUNT] = {
    4.5f,   // Dawn
    7.0f,   // Morning
    10.0f,  // Noon
    14.0f,  // Sunset
    19.0f,  // Dusk
    21.0f   // Night
};

static constexpr const char* ANCHOR_NAMES[ANCHOR_COUNT] = {
    "dawn", "morning", "noon", "sunset", "dusk", "night"
};

} // namespace sv
