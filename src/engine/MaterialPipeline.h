// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── MaterialPipeline ───────────────────────────────────────
// Creates per-material VkDescriptorSets from PBR textures and
// material factor overrides.  Provides fallback 1×1 textures for
// missing slots so every set is always fully bound.
// Layer 5 — depends on: VkTex (L1), VkDescriptors (L1), VkBuffer (L1)

#include "vk/VkTexture.h"
#include "vk/VkBuffer.h"
#include "vk/VkDescriptors.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace sv {

class VkCtx;
class VkMesh;
struct SceneNode;
struct MeshMaterial;

// ── Material constants (std140 UBO) ────────────────────────────────
struct MaterialConstants {
    glm::vec4 baseColor{1, 1, 1, 1};   // 16 bytes
    float     metallic  = 0.0f;         //  4
    float     roughness = 1.0f;         //  4
    float     _pad[2]   = {};           //  8 → total 32 bytes
};

// ── Per-node material descriptor set ───────────────────────────────
struct SceneMaterialSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkBuf           ubo;                // MaterialConstants upload
};

// ── MaterialPipeline ───────────────────────────────────────────────
// Owns the descriptor layout, pool, and 1×1 fallback textures.
// Games call createMaterialSet() for each SceneNode that references
// a mesh, then bind the returned set at draw time.
//
// Descriptor set layout (all fragment-stage):
//   binding 0 — uniform buffer  (MaterialConstants)
//   binding 1 — sampler2D       (baseColor)
//   binding 2 — sampler2D       (normal)
//   binding 3 — sampler2D       (metallicRoughness)
//   binding 4 — sampler2D       (emissive)
//   binding 5 — sampler2D       (occlusion)
//   binding 6 — sampler2D       (opacity)
class MaterialPipeline {
public:
    // Initialise layout, pool, and fallback textures.
    // maxMaterials sets the descriptor pool capacity.
    bool init(VkCtx& ctx, uint32_t maxMaterials = 256);

    // Create a fully-bound material set for a scene node.
    // If mesh is non-null, its first material's textures are used
    // where available; otherwise fallback textures fill every slot.
    SceneMaterialSet createMaterialSet(VkCtx& ctx,
                                       const SceneNode& node,
                                       const VkMesh* mesh);

    // Create a material set from a MeshMaterial and its owning mesh.
    // Uses the material's texture indices to bind textures from the mesh;
    // falls back to 1x1 defaults for missing slots.
    SceneMaterialSet createMaterialSet(VkCtx& ctx,
                                       const MeshMaterial& mat,
                                       const VkMesh& mesh);

    // Destroy the UBO inside a material set.
    // The descriptor set itself is freed when the pool is destroyed.
    void destroyMaterialSet(VmaAllocator alloc, SceneMaterialSet& ms);

    // Destroy pool, layout, and fallback textures.
    void destroy(VkCtx& ctx);

    VkDescriptorSetLayout layout() const { return m_layout; }

private:
    void createFallbackTextures(VkCtx& ctx);

    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescPool            m_pool;

    // Fallback 1×1 textures (indexed by TextureType ordinal)
    VkTex m_fallback[6];
};

} // namespace sv
