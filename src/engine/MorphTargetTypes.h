// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// Morph target types and GPU storage.
// VkMesh parses glTF morph targets into MorphTargetData.
// SSBO layout: vec4 pairs (posDelta, normDelta) per target per vertex.
// Vertex shader applies weighted morph blending before skinning.

#include "vk/VkBuffer.h"

#include <glm/glm.hpp>
#include <volk.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <string>
#include <cstdint>

namespace sv {

// Maximum morph targets blendable in the vertex shader.
// Face morphs exceeding this use a compute pre-pass.
constexpr uint32_t MAX_VERTEX_SHADER_MORPH_TARGETS = 8;

// Push constant layout shared by all skinned pipelines (engine + games).
// 128 bytes — Vulkan guaranteed minimum.
struct SkinnedPushConstants {
    glm::mat4 model;            // offset  0, 64B
    uint32_t  boneOffset;       // offset 64,  4B
    uint32_t  morphTargetCount; // offset 68,  4B
    uint32_t  vertexCount;      // offset 72,  4B
    uint32_t  alphaMode;        // offset 76,  4B — 0=opaque (alpha test), 1=alpha blend
    glm::vec4 tintColor;        // offset 80, 16B (fragment shader reads at 80)
    glm::vec4 morphWeights0;    // offset 96, 16B (weights 0-3)
    glm::vec4 morphWeights1;    // offset112, 16B (weights 4-7)
};
static_assert(sizeof(SkinnedPushConstants) == 128, "push constants must be 128 bytes");

// Info for a single morph target (shape key).
struct MorphTargetInfo {
    std::string name;
    float       defaultWeight = 0.0f;
};

// Per-mesh morph target data.  Owns the GPU SSBO and descriptor set.
//
// SSBO layout (std430, vec4 array):
//   For T targets and V vertices:
//     index = (t * vertexCount + v) * 2
//     morphDeltas[index + 0].xyz = position delta
//     morphDeltas[index + 1].xyz = normal delta
//
struct MorphTargetData {
    std::vector<MorphTargetInfo> targets;
    VkBuf                        deltaSSBO{};       // GPU storage buffer
    VkDescriptorPool             descPool  = VK_NULL_HANDLE;
    VkDescriptorSet              descSet   = VK_NULL_HANDLE;  // set 3
    uint32_t                     vertexCount = 0;

    bool     hasMorphTargets() const { return !targets.empty(); }
    uint32_t targetCount()     const { return (uint32_t)targets.size(); }

    // Create descriptor set from the given layout.  Call after deltaSSBO is uploaded.
    bool createDescriptorSet(VkDevice device, VkDescriptorSetLayout layout);

    void destroy(VkDevice device, VmaAllocator allocator);
};

// Create the shared descriptor set layout for morph target binding (set 3, binding 0 SSBO).
// Call once; reuse across all pipelines that support morph targets.
VkDescriptorSetLayout createMorphTargetDescSetLayout(VkDevice device);

} // namespace sv
