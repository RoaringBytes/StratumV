// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "../RenderPass.h"
#include "../vk/VkShader.h"
#include "../vk/VkTexture.h"

#include <glm/glm.hpp>
#include <vector>

namespace sv {

// Generic mesh binding for shadow casting — game provides vertex/index buffers.
// Replaces game-specific Terrain* dependency.
struct ShadowMeshBinding {
    VkBuffer  vertexBuffer = VK_NULL_HANDLE;
    VkBuffer  indexBuffer  = VK_NULL_HANDLE;
    uint32_t  indexCount   = 0;
};

// Skinned mesh binding for shadow pass.
// Each entry represents one skinned character with its bone palette offset.
struct SkinnedShadowBinding {
    VkBuffer  vertexBuffer = VK_NULL_HANDLE;
    VkBuffer  indexBuffer  = VK_NULL_HANDLE;
    uint32_t  indexCount   = 0;
    uint32_t  boneOffset   = 0;
    glm::mat4 model{1.0f};
};

class ShadowPass : public RenderPass {
public:
    bool init(VkCtx& ctx, const Config& cfg) override;
    void resize(uint32_t width, uint32_t height) override;
    void record(const FrameData& frame) override;
    void shutdown() override;
    bool checkReload() override;
    const char* name() const override { return "ShadowPass"; }

    // Set external dependencies (call after init, before first record)
    void setMeshBinding(const ShadowMeshBinding& binding) { m_mesh = binding; }
    void setPipelineLayout(VkPipelineLayout layout) { m_pipelineLayout = layout; }

    // Build pipeline (call after setPipelineLayout)
    void buildPipeline();

    // ── Skinned mesh support ───────────────────────────────────────
    // Call after init to enable skinned shadow casting.
    // skinnedLayout: pipeline layout with set 0 (scene) + set 1 (bone SSBO)
    //                + push constants (mat4 model + uint cascade + uint boneOffset).
    void setSkinnedPipelineLayout(VkPipelineLayout layout) { m_skinnedPipeLayout = layout; }
    void setBonePaletteDescSet(VkDescriptorSet set) { m_bonePaletteDescSet = set; }
    void buildSkinnedPipeline();

    // Per-frame: set the list of skinned meshes to shadow-cast.
    void setSkinnedBindings(const SkinnedShadowBinding* bindings, uint32_t count);
    void clearSkinnedBindings() { m_skinnedBindings.clear(); }

    // Access shadow map for binding in other passes
    const ShadowMap& shadowMap() const { return m_shadowMap; }

private:
    VkCtx*           m_ctx = nullptr;
    ShadowMeshBinding m_mesh;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

    ShadowMap       m_shadowMap;
    VkShader        m_vert;
    VkShader        m_frag;
    VkPipeline      m_pipeline = VK_NULL_HANDLE;
    uint32_t        m_mapSize = 4096;

    // ── Skinned shadow members ─────────────────────────────────────
    VkPipelineLayout m_skinnedPipeLayout  = VK_NULL_HANDLE;
    VkPipeline       m_skinnedPipeline    = VK_NULL_HANDLE;
    VkShader         m_skinnedVert;
    VkDescriptorSet  m_bonePaletteDescSet = VK_NULL_HANDLE;
    std::vector<SkinnedShadowBinding> m_skinnedBindings;
};

} // namespace sv
