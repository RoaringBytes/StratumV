// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// SceneUBO matching skinned.frag + skinnedPBR.frag SceneData declaration.
// std140 layout — 256 B original sun/ambient block + 528 B light tail
// + 16 B material override = 800 bytes total as of .
//
// ── Layout map ───────────────────────────────────────────────────
//   offset  0   viewProj              mat4
//   offset 64   invViewProj           mat4
//   offset 128  invViewProjUnjittered mat4
//   offset 192  cameraPos             vec4
//   offset 208  sunDirection          vec4   .w = sunHeight
//   offset 224  sunColor              vec4   .w = sunIntensity
//   offset 240  ambientColor          vec4
// offset 256 lightCount uvec4 .x = count 0..8
// offset 272 lights[8] Light[8]
// offset 784 materialOverride vec4 .rgb = tint, .a = strength
//
// The original 0..255 block is byte-identical to 1.3.8 — consumers
// that don't adopt dynamic lights see the same sun/ambient data at
// the same offsets and read zero lights at the new tail.
//
// The material override is a single global vec4 the
// fragment shader multiplies into the per-mesh basecolor at the very
// end of the lighting pipeline:
//   finalRGB = mix(finalRGB, finalRGB * override.rgb, override.a);
// Default (1, 1, 1, 0) leaves rendering byte-identical to 1.3.9 (the
// strength gate is the alpha — 0 means "skip the mix entirely"). The
// lab harness picks the FIRST replicated entity whose
// MaterialComponent has overrideStrength > 0 (sorted by entityId)
// each frame, packs it into this slot, and uploads. A future
// session can graduate this to a per-entity material descriptor set
// when the SkinnedMeshPass dispatch path is rewritten.
//
// ── Per-Light std140 layout ──────────────────────────────────────
//   offset  0   positionType    vec4   .xyz = world pos, .w = type (float-cast)
//   offset 16   directionRange  vec4   .xyz = unit direction, .w = range
//   offset 32   colorIntensity  vec4   .xyz = color,     .w = intensity
//   offset 48   coneParams      vec4   .x = cos(innerDeg), .y = cos(outerDeg),
//                                       .z = enabled flag, .w = pad
// 64 bytes per Light × 8 = 512 bytes. Plus the 16 B lightCount header
// = 528 bytes appended to the original 256 B.
//
// ── Light cap rationale ──────────────────────────────────────────
// MAX_LIGHTS = 8 is intentionally cheap — enough for a visual
// checkpoint and collaborative editing demos, small enough to keep
// the shader light loop cost trivial. A later perf session can
// graduate this to a light SSBO + clustered/tiled renderer when the
// world scales beyond an editor scene.

#include <glm/glm.hpp>

namespace sv_ubo {
constexpr uint32_t kMaxLights = 8;
} // namespace sv_ubo

struct SceneLight {
    glm::vec4 positionType    = glm::vec4(0.0f);
    glm::vec4 directionRange  = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
    glm::vec4 colorIntensity  = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    glm::vec4 coneParams      = glm::vec4(1.0f, 0.5f, 0.0f, 0.0f);
};
static_assert(sizeof(SceneLight) == 64,
              "SceneLight must be 64 bytes to match std140 layout");

struct SceneUBO {
    glm::mat4  viewProj;                       // offset 0
    glm::mat4  invViewProj;                    // offset 64
    glm::mat4  invViewProjUnjittered;          // offset 128
    glm::vec4  cameraPos;                      // offset 192
    glm::vec4  sunDirection;                   // offset 208   .w = sunHeight
    glm::vec4  sunColor;                       // offset 224   .w = sunIntensity
    glm::vec4  ambientColor;                   // offset 240
    glm::uvec4 lightCount;                     // offset 256   .x = count, .yzw pad
    SceneLight lights[sv_ubo::kMaxLights];     // offset 272
    glm::vec4 materialOverride = // offset 784
        glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
};
static_assert(sizeof(SceneUBO) == 800,
              "SceneUBO must be 800 bytes (std140) after "
              "material override extension");
static_assert(offsetof(SceneUBO, lightCount) == 256,
              "SceneUBO sun/ambient block must stay byte-identical at "
              "offsets 0..255 — light tail is strictly additive");
static_assert(offsetof(SceneUBO, lights) == 272,
              "SceneUBO::lights offset drift — std140 expects uvec4 to "
              "occupy offsets 256..271 with lights[] starting at 272");
static_assert(offsetof(SceneUBO, materialOverride) == 784,
              "SceneUBO::materialOverride offset drift — std140 expects "
              "vec4 to land at offset 784 directly after lights[7]");
