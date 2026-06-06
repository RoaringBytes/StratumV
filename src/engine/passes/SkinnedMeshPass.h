// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// Skinned mesh rendering utility (with morph target support).
// Not a standalone RenderPass — games call bind()/draw() from within
// their own main pass recording.  Owns its pipeline layout (set 0 scene +
// set 1 bone palette + set 2 material + set 3 morph targets) and graphics pipeline.

#include "../RenderPass.h"
#include "../vk/VkShader.h"
#include "../vk/VkBuffer.h"
#include "../MorphTargetTypes.h"

#include <glm/glm.hpp>
#include <volk.h>
#include <vk_mem_alloc.h>
#include <cstdint>

namespace sv {

class VkCtx;

// SkinnedPushConstants (128B) is defined in MorphTargetTypes.h.

// Per-draw command for a skinned submesh.
struct SkinnedDrawCmd {
    VkBuffer        vertexBuffer      = VK_NULL_HANDLE;
    VkBuffer        indexBuffer       = VK_NULL_HANDLE;
    uint32_t        indexCount        = 0;
    uint32_t        firstIndex        = 0;    // offset into index buffer (per-submesh)
    glm::mat4       model{1.0f};
    uint32_t        boneOffset        = 0;    // from AnimationSystem::uploadBones()
    VkDescriptorSet materialSet       = VK_NULL_HANDLE;  // set 2 (MaterialPipeline)
    VkDescriptorSet morphTargetSet    = VK_NULL_HANDLE;  // set 3 (morph targets, null → dummy)
    uint32_t        morphTargetCount  = 0;
    uint32_t        vertexCount       = 0;
    glm::vec4       tintColor{1.0f};
    float           morphWeights[MAX_VERTEX_SHADER_MORPH_TARGETS]{};
};

class SkinnedMeshPass {
public:
    // Initialize: creates pipeline layout, loads shaders, builds pipeline.
    // sceneDescLayout: set 0, bonePaletteLayout: set 1, materialLayout: set 2.
    // pipelineCache: optional VkPipelineCache handle forwarded to
    // the builder to reuse a driver-cached pipeline blob.
    bool init(VkCtx& ctx, VkDescriptorSetLayout sceneDescLayout,
              VkDescriptorSetLayout bonePaletteLayout,
              VkDescriptorSetLayout materialLayout, VkFormat colorFormat,
              VkPipelineCache pipelineCache = VK_NULL_HANDLE);
    void destroy();

    // Hot-reload: returns true if shaders were recompiled (caller should
    // re-bind pipeline on next frame). Reuses the pipeline cache set at
    // init time.
    bool checkReload();

    // ── Recording API (call from within main pass) ─────────────────

    // Bind opaque pipeline + per-frame descriptor sets (scene + bones).
    // Call once per frame before drawing opaque submeshes.
    void bind(VkCommandBuffer cmd, VkDescriptorSet sceneDescSet,
              VkDescriptorSet bonePaletteDescSet);

    // Bind alpha-blend pipeline + per-frame descriptor sets.
    // Call after opaque draws, before drawing transparent submeshes.
    void bindBlend(VkCommandBuffer cmd, VkDescriptorSet sceneDescSet,
                   VkDescriptorSet bonePaletteDescSet);

    // Issue a single indexed draw with per-draw push constants.
    // Binds materialSet (set 2) and morphTargetSet (set 3) if non-null.
    // alphaBlend: true = output actual alpha (for blend pipeline).
    void draw(VkCommandBuffer cmd, const SkinnedDrawCmd& drawCmd,
              bool alphaBlend = false);

    // ── Accessors ──────────────────────────────────────────────────
    VkPipelineLayout        pipelineLayout()    const { return m_pipelineLayout; }
    VkPipeline              pipeline()          const { return m_pipeline; }
    VkDescriptorSetLayout   morphTargetLayout() const { return m_morphTargetLayout; }
    VkDescriptorSet         dummyMorphSet()     const { return m_dummyMorphDescSet; }

private:
    VkCtx*           m_ctx = nullptr;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;
    VkPipeline       m_pipelineBlend  = VK_NULL_HANDLE;  // alpha-blend
    VkShader         m_vert;
    VkShader         m_frag;
    VkFormat         m_colorFormat    = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Morph target descriptor resources
    VkDescriptorSetLayout m_morphTargetLayout  = VK_NULL_HANDLE;
    VkBuf                 m_dummyMorphSSBO{};
    VkDescriptorPool      m_dummyMorphPool     = VK_NULL_HANDLE;
    VkDescriptorSet       m_dummyMorphDescSet  = VK_NULL_HANDLE;

    // Pipeline cache handle set at init() time; reused on
    // shader hot-reload via checkReload().
    VkPipelineCache       m_pipelineCache      = VK_NULL_HANDLE;
};

} // namespace sv
