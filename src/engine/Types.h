// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <glm/glm.hpp>
#include <cstdint>

// Forward declarations for pipeline builders
namespace sv {
struct MeshVertex;
struct TerrainVertex;
class VkPipeBuilder;
}

namespace sv {

// ── Halton low-discrepancy sequence for sub-pixel jitter (DLSS) ──
inline float halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

// ── Per-frame UBO (shared by all pipelines, std140) ─────────────
struct SceneUBO {
    glm::mat4 viewProj;             //   0
    glm::mat4 invViewProj;          //  64
    glm::mat4 invViewProjUnjittered; // 128  (no DLSS jitter — for CPU raycasting)
    glm::vec4 cameraPos;            // 192  (.w unused)
    glm::vec4 sunDirection;         // 192  (.w = sunHeight)
    glm::vec4 sunColor;             // 208  (.w = sunIntensity)
    glm::vec4 ambientColor;         // 224  (.w unused)
    float     time;                 // 240
    float     _pad0[3];             // 244
    // Sky tunables
    glm::vec4 skyZenith;            // 256
    glm::vec4 skyHorizon;           // 272
    glm::vec4 skyMid;               // 288
    glm::vec4 nadirColor;           // 304
    glm::vec4 wideGlowColor;       // 320  (.w = wideGlowStrength)
    glm::vec4 cloudBrightColor;     // 336
    glm::vec4 cloudShadowColor;     // 352
    glm::vec4 cloudGlowColor;       // 368
    glm::vec4 sunsetWarmColor;      // 384
    glm::vec4 sunsetPinkColor;      // 400
    glm::vec4 hazeColorDay;         // 416
    glm::vec4 hazeColorSunset;      // 432
    glm::vec4 godRayColor;          // 448  (.w = godRayIntensity)
    float     cloudDensity;         // 464
    float     cloudSpeed;           // 468
    float     skyHaze;              // 472
    float     sunGlowPower;         // 476
    float     sunDiskSize;          // 480
    float     _pad1[3];             // 484
    // Shadow (cascaded)
    glm::mat4 lightViewProj[3];     // 496  (3 cascades)
    glm::vec4 splitDistances;       // 688  (.xyz = cascade far distances)
    float     shadowBiasMin;        // 704
    float     shadowBiasMax;        // 708
    float     shadowIntensity;      // 712
    float     shadowNormalOffset;   // 716
    float     shadowPcfRadius;      // 720
    float     shadowEdgeFade;       // 724
    float     _pad2[2];            // 728
    // Temporal (DLSS)
    glm::mat4 prevViewProj;        // 736
    float     jitterX;             // 800
    float     jitterY;             // 804
    uint32_t  frameIndex;          // 808
    uint32_t  rtShadowEnabled;     // 812
    // ReSTIR DI
    uint32_t  lightCount;          // 816
    uint32_t  restirEnabled;       // 820
    uint32_t  sharcEnabled;         // 824
    uint32_t  _pad3;               // 828  (align to 832)
    // Fog
    glm::vec4 fogColor;            // 832  (.xyz = color)
    float     fogDensity;          // 848
    float     fogStart;            // 852
    float     fogEnd;              // 856
    float     _padFog;             // 860  (align to 864)
    // ── Sky & Cloud tuning ──
    // Cloud geometry
    float     cloudHeight1;        // 864  layer 1 ray height
    float     cloudScale1;         // 868  layer 1 UV scale
    float     cloudDetail1;        // 872  layer 1 FBM frequency
    float     cloudSoftness;       // 876  smoothstep edge width
    float     cloudHeight2;        // 880  layer 2 (wisp) height
    float     cloudScale2;         // 884  layer 2 UV scale
    float     cloudDetail2;        // 888  layer 2 FBM frequency
    float     cloudOpacity2;       // 892  layer 2 max opacity
    float     cloudLayerBlend1;    // 896  layer 1 composite alpha
    float     cloudLayerBlend2;    // 900  layer 2 composite alpha
    float     cloudFadeStart;      // 904  horizon fade start
    float     cloudFadeEnd;        // 908  horizon fade end
    // Sun & glow
    float     sunGlowMultiplier;   // 912  post-glow brightness
    float     wideGlowExponent;    // 916  wide atmospheric glow sharpness
    float     cloudSunEdgeExp;     // 920  cloud golden-edge exponent
    float     cloudMinLighting;    // 924  cloud shadow floor
    // God rays
    float     godRayDensity;       // 928  streak angular frequency scale
    float     godRayFalloff;       // 932  radial falloff exponent
    float     godRayMultiplier;    // 936  final ray color multiplier
    float     godRaySpread;        // 940  angular spread scaling
    // Atmosphere
    float     sunsetStartHeight;   // 944  sun height where sunset begins
    float     sunsetWarmStrength;   // 948  warm color horizon intensity
    float     sunsetPinkStrength;   // 952  pink color band intensity
    float     nightDarkness;        // 956  minimum sky brightness at night
    float     hazeFalloff;          // 960  haze height falloff exponent
    float     saturationBoost;      // 964  color saturation multiplier
    float     skyGradientExponent;  // 968  sky height-to-color curve power
    float     skyGradientSplit;     // 972  horizon/mid transition point
    // God ray shape
    float     godRayLength;          // 976  radial extent multiplier
    float     godRayWidth;           // 980  streak angular width multiplier
    float     godRayCount;           // 984  ray count multiplier
    float     godRayAsymmetry;       // 988  ray unevenness (0=even, 1=dominant few)
    // Night Sky
    glm::vec4 moonDirection;           // 992  .xyz = dir, .w = moonPhase (0=new..1)
    glm::vec4 moonColor;              // 1008 .xyz = color, .w = moonIntensity
    float     moonDiskSize;           // 1024
    float     moonGlowPower;         // 1028
    float     moonAmbientContrib;    // 1032
    float     _padMoon;              // 1036
    float     starBrightness;        // 1040
    float     starDensity;           // 1044
    float     starTwinkleSpeed;      // 1048
    float     starTwinkleAmount;     // 1052
    float     milkyWayStrength;      // 1056
    float     milkyWayTilt;          // 1060
    float     nightFadeThreshold;    // 1064
    float     _padNight;             // 1068
};                                    // Total: 1072 bytes
// sizeof(SceneUBO) verified at compile time — update if layout changes
static_assert(sizeof(SceneUBO) == 1088, "SceneUBO size mismatch — check std140 layout");

// ── Terrain UBO (std140, all scalar floats = no padding needed) ──
struct TerrainUBO {
    // Height thresholds
    float sandEnd;             //  0
    float mudStart;            //  4
    float mudEnd;              //  8
    float dirtStart;           // 12
    float dirtEnd;             // 16
    float grassStart;          // 20
    float grassFull;           // 24
    float grassEnd;            // 28
    float gravelStart;         // 32
    float gravelEnd;           // 36
    float rockStart;           // 40
    float rockFull;            // 44
    // Tile scales
    float sandTileScale;       // 48
    float mudTileScale;        // 52
    float dirtTileScale;       // 56
    float grassTileScale;      // 60
    float gravelTileScale;     // 64
    float rockTileScale;       // 68
    // Blend noise
    float blendNoiseScale;     // 72
    float blendNoiseStrength;  // 76
    // Slope
    float slopeRockMin;        // 80
    float slopeRockMax;        // 84
    float slopeRockHeight;     // 88
    float slopeNoiseStrength;  // 92
    // POM
    float pomScale;            // 96
    float pomSteps;            // 100
    float pomFadeNear;         // 104
    float pomFadeFar;          // 108
    float normalStrength;      // 112
    // Micro-blend
    float transitionWidth;     // 116
    float microBlendStrength;  // 120
    float microBlendContrast;  // 124
    float _pad1;               // 128
    // Environment depth — terrain center/size for UV computation in shader
    // NOTE: vec4 needs 16-byte alignment in std140. Offset 132 is NOT aligned.
    // Add 3 more padding floats to reach offset 144.
    float _pad1b, _pad1c, _pad1d; // 132, 136, 140
    glm::vec4 envTerrainParams; // 144: x=centerX, y=centerZ, z=worldSize, w=waterLevel
    // Seabed effects (offset 160+)
    float seabedBoostIntensity;  // 160
    float envCausticIntensity;   // 164
    float envCausticMaxDepth;    // 168
    float envCausticScale;       // 172
    float envCausticSpeed;       // 176
    float underwaterFogDensity;  // 180
    float underwaterTintDepth;   // 184
    float _pad2;                 // 188
    glm::vec4 underwaterAbsorption; // 192: rgb = absorption coefficients
    glm::vec4 underwaterFogColor;   // 208: rgb = fog color
    glm::vec4 envCausticColor;      // 224: rgb = caustic color
    // Tessellation
    float tessTargetEdge   = 14.0f;  // 240: target screen-space edge pixels
    float tessMaxLevel     = 64.0f;  // 244: max tessellation factor
    float heightmapTexelSize = 0.0f; // 248: 1.0/heightmapRes (set at runtime)
    float terrainWorldSize = 0.0f;   // 252: world extent (set at runtime)
    glm::vec4 terrainOrigin{0.f};    // 256: .xy=center.xz, .z=minH, .w=maxH
};                              // Total: 272 bytes (std140)

inline void fillTerrainDefaults(TerrainUBO& t)
{
    t.sandEnd       = 1.0f;
    t.mudStart      = -0.5f;  t.mudEnd     = 1.5f;
    t.dirtStart     = 1.5f;   t.dirtEnd    = 3.5f;
    t.grassStart    = 3.0f;   t.grassFull  = 5.0f;  t.grassEnd = 15.0f;
    t.gravelStart   = 12.0f;  t.gravelEnd  = 18.0f;
    t.rockStart     = 15.0f;  t.rockFull   = 25.0f;
    // Tile scales
    t.sandTileScale   = 0.1f;
    t.mudTileScale    = 0.08f;
    t.dirtTileScale   = 0.09f;
    t.grassTileScale  = 0.08f;
    t.gravelTileScale = 0.1f;
    t.rockTileScale   = 0.06f;
    // Blend noise
    t.blendNoiseScale    = 0.05f;
    t.blendNoiseStrength = 2.0f;
    // Slope
    t.slopeRockMin       = 0.25f;
    t.slopeRockMax       = 0.6f;
    t.slopeRockHeight    = 2.0f;
    t.slopeNoiseStrength = 0.18f;
    // POM
    t.pomScale     = 0.04f;
    t.pomSteps     = 8.0f;
    t.pomFadeNear  = 10.0f;
    t.pomFadeFar   = 25.0f;
    t.normalStrength = 1.0f;
    // Micro-blend
    t.transitionWidth     = 3.0f;
    t.microBlendStrength  = 0.5f;
    t.microBlendContrast  = 4.0f;
    t._pad1 = 0.f; t._pad1b = 0.f; t._pad1c = 0.f; t._pad1d = 0.f;
    // Environment depth
    t.envTerrainParams      = glm::vec4(0.f, 0.f, 1.f, 0.f); // filled at runtime
    t.seabedBoostIntensity  = 0.4f;
    t.envCausticIntensity   = 0.5f;
    t.envCausticMaxDepth    = 8.0f;
    t.envCausticScale       = 8.0f;
    t.envCausticSpeed       = 0.6f;
    t.underwaterFogDensity  = 0.06f;
    t.underwaterTintDepth   = 5.0f;
    t._pad2 = 0.f;
    t.underwaterAbsorption  = glm::vec4(0.45f, 0.08f, 0.02f, 0.f);  // red fades first
    t.underwaterFogColor    = glm::vec4(0.02f, 0.08f, 0.15f, 0.f);
    t.envCausticColor       = glm::vec4(0.8f, 0.9f, 1.0f, 0.f);
    // Tessellation
    t.tessTargetEdge     = 14.0f;
    t.tessMaxLevel       = 64.0f;
    t.heightmapTexelSize = 0.0f;  // set at runtime
    t.terrainWorldSize   = 0.0f;  // set at runtime
    t.terrainOrigin      = glm::vec4(0.f);
}

// ── GPU light (std430 for SSBO, 64 bytes per light) ─────────────
struct GPULight {
    glm::vec4 positionAndType;     // xyz=position, w=type (0=dir,1=point,2=rect,3=disk)
    glm::vec4 directionAndRadius;  // xyz=direction, w=radius(point) or half-width(rect)
    glm::vec4 colorAndIntensity;   // xyz=color, w=intensity
    glm::vec4 dimensions;          // xy=rect half-extents or disk radius, zw=reserved
};
static_assert(sizeof(GPULight) == 64);

static constexpr uint32_t MAX_LIGHTS = 256;

// ── GPU-driven instance data ──────────────────────────────
struct GpuInstance {
    glm::mat4 model;           // 64B: world transform
    glm::vec4 boundingSphere;  // 16B: center.xyz + radius.w
    uint32_t  meshId;          //  4B: index into mesh registry
    uint32_t  materialId;      //  4B: material index
    uint32_t  _pad[2];         //  8B: align to 96
};
static_assert(sizeof(GpuInstance) == 96, "GpuInstance must be 96 bytes for std430");
static constexpr uint32_t MAX_GPU_INSTANCES = 65536;

// ── Mesh Registry ────────────────────────────────────────
// Per-mesh metadata for multi-mesh indirect draw.
// Stored in GPU SSBO so compute + vertex shaders can look up offsets.
struct MeshSlot {
    uint32_t indexCount;       // number of indices for this mesh
    uint32_t firstIndex;       // offset into mega-IBO (in indices, not bytes)
    int32_t  vertexOffset;     // offset into mega-VBO (in vertices, not bytes)
    float    boundingSphereR;  // default bounding sphere radius for this mesh type
};
static_assert(sizeof(MeshSlot) == 16, "MeshSlot must be 16 bytes for std430");
static constexpr uint32_t MAX_MESH_TYPES = 256;

// Simple material for GPU-driven rendering
struct GpuMaterial {
    glm::vec4 albedo;          // rgb + alpha
};
static_assert(sizeof(GpuMaterial) == 16, "GpuMaterial must be 16 bytes for std430");
static constexpr uint32_t MAX_GPU_MATERIALS = 256;

// ── ReSTIR push constants ────────────────────────────────────────
struct ReSTIRPushConstants {
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t frameIndex;
};

// ── SHaRC push constants ────────────────────────────────────────
struct SharcPushConstants {
    float    camX, camY, camZ;
    float    logBase;
    float    sceneScale;
    float    levelBias;
    uint32_t capacity;
    uint32_t staleFrameMax;
    uint64_t hashEntriesAddr;
    uint64_t accumulationAddr;
    uint64_t resolvedAddr;
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t frameIndex;
    uint32_t _pad;
};
static constexpr uint32_t SHARC_CAPACITY = 1u << 22; // 4M entries

// ── Per-frame sync ──────────────────────────────────────────────
struct FrameSync {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;
    VkSemaphore     renderFinished = VK_NULL_HANDLE;
    VkFence         inFlight       = VK_NULL_HANDLE;
};

// ── Image layout transition ─────────────────────────────────────
inline void transitionImage(VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkAccessFlags srcAccess, VkAccessFlags dstAccess,
    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { aspect, 0, 1, 0, 1 };

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0,
        0, nullptr, 0, nullptr, 1, &barrier);
}

// ── Fill UBO with default sky values ────────────────────────────
inline void fillSkyDefaults(SceneUBO& ubo)
{
    // Sun
    glm::vec3 sunDir = glm::normalize(glm::vec3(0.4f, 0.6f, -0.3f));
    ubo.sunDirection = glm::vec4(sunDir, sunDir.y);
    ubo.sunColor     = glm::vec4(1.0f, 0.95f, 0.85f, 5.0f); // .w = intensity
    ubo.ambientColor = glm::vec4(0.15f, 0.18f, 0.25f, 0.0f);

    // Sky gradient
    ubo.skyZenith    = glm::vec4(0.08f, 0.18f, 0.55f, 0.0f);
    ubo.skyHorizon   = glm::vec4(0.45f, 0.65f, 0.85f, 0.0f);
    ubo.skyMid       = glm::vec4(0.20f, 0.40f, 0.75f, 0.0f);
    ubo.nadirColor   = glm::vec4(0.02f, 0.04f, 0.08f, 0.0f);

    // Sun glow
    ubo.wideGlowColor = glm::vec4(1.0f, 0.7f, 0.3f, 0.35f); // .w = strength
    ubo.sunGlowPower  = 32.0f;
    ubo.sunDiskSize    = 0.005f;

    // Clouds
    ubo.cloudBrightColor = glm::vec4(1.0f, 0.98f, 0.95f, 0.0f);
    ubo.cloudShadowColor = glm::vec4(0.35f, 0.40f, 0.50f, 0.0f);
    ubo.cloudGlowColor   = glm::vec4(1.0f, 0.85f, 0.5f, 0.0f);
    ubo.cloudDensity      = 0.5f;
    ubo.cloudSpeed        = 1.0f;

    // Sunset
    ubo.sunsetWarmColor = glm::vec4(1.0f, 0.45f, 0.15f, 0.0f);
    ubo.sunsetPinkColor = glm::vec4(0.9f, 0.4f, 0.6f, 0.0f);

    // Haze
    ubo.hazeColorDay    = glm::vec4(0.65f, 0.78f, 0.90f, 0.0f);
    ubo.hazeColorSunset = glm::vec4(1.0f, 0.6f, 0.3f, 0.0f);
    ubo.skyHaze         = 0.35f;

    // God rays
    ubo.godRayColor = glm::vec4(1.0f, 0.9f, 0.6f, 0.5f); // .w = intensity

    // Shadow defaults (lightViewProj computed per-frame in render loop)
    ubo.shadowBiasMin      = 0.003f;
    ubo.shadowBiasMax      = 0.01f;
    ubo.shadowIntensity    = 1.0f;
    ubo.shadowNormalOffset = 0.05f;
    ubo.shadowPcfRadius    = 1.0f;  // 3x3 PCF
    ubo.shadowEdgeFade     = 0.1f;

    // Fog (off by default)
    ubo.fogColor   = glm::vec4(0.7f, 0.75f, 0.82f, 0.0f);
    ubo.fogDensity = 0.0f;
    ubo.fogStart   = 50.0f;
    ubo.fogEnd     = 500.0f;

    // Cloud shape
    ubo.cloudHeight1      = 800.0f;
    ubo.cloudScale1       = 0.00018f;
    ubo.cloudDetail1      = 2.2f;
    ubo.cloudSoftness     = 0.24f;
    ubo.cloudHeight2      = 1500.0f;
    ubo.cloudScale2       = 0.00012f;
    ubo.cloudDetail2      = 3.0f;
    ubo.cloudOpacity2     = 0.4f;
    ubo.cloudLayerBlend1  = 0.8f;
    ubo.cloudLayerBlend2  = 0.5f;
    ubo.cloudFadeStart    = 0.01f;
    ubo.cloudFadeEnd      = 0.15f;
    // Sun & glow
    ubo.sunGlowMultiplier = 0.6f;
    ubo.wideGlowExponent  = 8.0f;
    ubo.cloudSunEdgeExp   = 4.0f;
    ubo.cloudMinLighting  = 0.35f;
    // God rays
    ubo.godRayDensity     = 1.0f;
    ubo.godRayFalloff     = 3.5f;
    ubo.godRayMultiplier  = 0.5f;
    ubo.godRaySpread      = 1.0f;
    // Atmosphere
    ubo.sunsetStartHeight  = 0.3f;
    ubo.sunsetWarmStrength  = 0.65f;
    ubo.sunsetPinkStrength  = 0.25f;
    ubo.nightDarkness       = 0.08f;
    ubo.hazeFalloff         = 5.0f;
    ubo.saturationBoost     = 1.2f;
    ubo.skyGradientExponent = 0.45f;
    ubo.skyGradientSplit    = 0.4f;
    // God ray shape
    ubo.godRayLength        = 1.0f;
    ubo.godRayWidth         = 1.0f;
    ubo.godRayCount         = 1.0f;
    ubo.godRayAsymmetry     = 0.0f;
    // Night Sky
    ubo.moonDirection       = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
    ubo.moonColor           = glm::vec4(0.8f, 0.85f, 0.95f, 1.0f);
    ubo.moonDiskSize        = 0.004f;
    ubo.moonGlowPower      = 12.0f;
    ubo.moonAmbientContrib  = 0.05f;
    ubo._padMoon            = 0.0f;
    ubo.starBrightness      = 0.0f;
    ubo.starDensity         = 400.0f;
    ubo.starTwinkleSpeed    = 1.0f;
    ubo.starTwinkleAmount   = 0.3f;
    ubo.milkyWayStrength    = 0.0f;
    ubo.milkyWayTilt        = 0.4f;
    ubo.nightFadeThreshold  = 0.05f;
    ubo._padNight           = 0.0f;
}

// ── Pipeline builders ───────────────────────────────────────────
// These need VkPipeline.h and VkMesh.h includes at the call site.
// each builder accepts an optional VkPipelineCache that is
// forwarded to VkPipeBuilder::build; pass VK_NULL_HANDLE to skip
// persistent pipeline caching (the default for backward compat).

VkPipeline buildSkyPipeline(VkDevice device,
    VkShaderModule vert, VkShaderModule frag,
    VkPipelineLayout layout, VkFormat colorFormat,
    VkPipelineCache cache = VK_NULL_HANDLE);

VkPipeline buildScenePipeline(VkDevice device,
    VkShaderModule vert, VkShaderModule frag,
    VkPipelineLayout layout, VkFormat colorFormat,
    VkPipelineCache cache = VK_NULL_HANDLE);

VkPipeline buildTerrainPipeline(VkDevice device,
    VkShaderModule vert, VkShaderModule frag,
    VkPipelineLayout layout, VkFormat colorFormat);

VkPipeline buildTerrainTessPipeline(VkDevice device,
    VkShaderModule vert, VkShaderModule frag,
    VkShaderModule tesc, VkShaderModule tese,
    VkPipelineLayout layout, VkFormat colorFormat);

VkPipeline buildShadowPipeline(VkDevice device,
    VkShaderModule vert, VkShaderModule frag,
    VkPipelineLayout layout);

// Skinned mesh pipeline — same vertex layout as scene but uses bone palette SSBO
VkPipeline buildSkinnedMeshPipeline(VkDevice device,
    VkShaderModule vert, VkShaderModule frag,
    VkPipelineLayout layout, VkFormat colorFormat,
    VkPipelineCache cache = VK_NULL_HANDLE);

// Skinned mesh alpha-blend pipeline — no cull, no depth write, blend on
VkPipeline buildSkinnedMeshPipelineBlend(VkDevice device,
    VkShaderModule vert, VkShaderModule frag,
    VkPipelineLayout layout, VkFormat colorFormat,
    VkPipelineCache cache = VK_NULL_HANDLE);

// Skinned shadow pipeline — depth-only with bone palette SSBO
VkPipeline buildSkinnedShadowPipeline(VkDevice device,
    VkShaderModule vert, VkShaderModule frag,
    VkPipelineLayout layout,
    VkPipelineCache cache = VK_NULL_HANDLE);

} // namespace sv
