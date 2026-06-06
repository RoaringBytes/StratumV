// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── MaterialPipeline ───────────────────────────────────────
// Layer 5 — per-material descriptor sets for PBR rendering

#include "MaterialPipeline.h"
#include "SceneLoader.h"
#include "EngineLog.h"
#include "vk/VkContext.h"
#include "vk/VkMesh.h"

namespace sv {

static constexpr const char* TAG = "MaterialPipeline";

// ── init ───────────────────────────────────────────────────────────

bool MaterialPipeline::init(VkCtx& ctx, uint32_t maxMaterials)
{
    VkDevice device = ctx.device();

    // Descriptor set layout: 1 UBO + 6 combined image samplers
    m_layout = VkDescLayoutBuilder()
        .addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    VK_SHADER_STAGE_FRAGMENT_BIT)
        .addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    VK_SHADER_STAGE_FRAGMENT_BIT)   // baseColor
        .addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    VK_SHADER_STAGE_FRAGMENT_BIT)   // normal
        .addBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    VK_SHADER_STAGE_FRAGMENT_BIT)   // metallicRoughness
        .addBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    VK_SHADER_STAGE_FRAGMENT_BIT)   // emissive
        .addBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    VK_SHADER_STAGE_FRAGMENT_BIT)   // occlusion
        .addBinding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    VK_SHADER_STAGE_FRAGMENT_BIT)   // opacity
        .build(device);

    if (!m_layout) {
        SV_LOG_ERROR(TAG, "Failed to create material descriptor set layout");
        return false;
    }

    // Pool sizes: 1 UBO + 6 samplers per set
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         maxMaterials },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxMaterials * 6 },
    };

    if (!m_pool.init(device, maxMaterials, poolSizes, 2)) {
        SV_LOG_ERROR(TAG, "Failed to create material descriptor pool");
        return false;
    }

    createFallbackTextures(ctx);

    SV_LOG_INFO(TAG, "Initialised — pool capacity %u materials", maxMaterials);
    return true;
}

// ── Fallback textures ──────────────────────────────────────────────

void MaterialPipeline::createFallbackTextures(VkCtx& ctx)
{
    // baseColor: white, sRGB
    const uint8_t white[] = { 255, 255, 255, 255 };
    m_fallback[static_cast<int>(TextureType::baseColor)]
        .loadFromMemory(ctx, white, 1, 1, true);

    // normal: flat (0.5, 0.5, 1.0) → encoded as (128, 128, 255), linear
    const uint8_t normal[] = { 128, 128, 255, 255 };
    m_fallback[static_cast<int>(TextureType::normal)]
        .loadFromMemory(ctx, normal, 1, 1, false);

    // metallicRoughness: white → factors pass through, linear
    m_fallback[static_cast<int>(TextureType::metallicRoughness)]
        .loadFromMemory(ctx, white, 1, 1, false);

    // emissive: black, sRGB → no emission
    const uint8_t black[] = { 0, 0, 0, 255 };
    m_fallback[static_cast<int>(TextureType::emissive)]
        .loadFromMemory(ctx, black, 1, 1, true);

    // occlusion: white → fully unoccluded, linear
    m_fallback[static_cast<int>(TextureType::occlusion)]
        .loadFromMemory(ctx, white, 1, 1, false);

    // opacity: white → fully opaque, linear
    m_fallback[static_cast<int>(TextureType::opacity)]
        .loadFromMemory(ctx, white, 1, 1, false);
}

// ── createMaterialSet ──────────────────────────────────────────────

SceneMaterialSet MaterialPipeline::createMaterialSet(
    VkCtx& ctx, const SceneNode& node, const VkMesh* mesh)
{
    VkDevice device = ctx.device();
    SceneMaterialSet ms;

    // Allocate descriptor set
    ms.set = m_pool.allocate(device, m_layout);
    if (!ms.set) {
        SV_LOG_ERROR(TAG, "Failed to allocate material descriptor set for '%s'",
                     node.name.c_str());
        return ms;
    }

    // Upload material constants
    MaterialConstants constants;
    constants.baseColor = node.baseColor;
    constants.metallic  = node.metallic;
    constants.roughness = node.roughness;

    ms.ubo = VkBuf::createWithData(ctx, &constants, sizeof(constants),
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    // Resolve textures: mesh material first, fallback if missing
    const MeshMaterial* meshMat = nullptr;
    if (mesh && !mesh->materials().empty())
        meshMat = &mesh->materials()[0];

    // Map TextureType ordinal → texture index from mesh material
    int texIndices[6] = { -1, -1, -1, -1, -1, -1 };
    if (meshMat) {
        texIndices[static_cast<int>(TextureType::baseColor)]         = meshMat->baseColorTex;
        texIndices[static_cast<int>(TextureType::normal)]            = meshMat->normalTex;
        texIndices[static_cast<int>(TextureType::metallicRoughness)] = meshMat->metallicRoughnessTex;
        texIndices[static_cast<int>(TextureType::emissive)]          = meshMat->emissiveTex;
        texIndices[static_cast<int>(TextureType::occlusion)]         = meshMat->occlusionTex;
        texIndices[static_cast<int>(TextureType::opacity)]           = meshMat->opacityTex;
    }

    // Build descriptor writes
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = ms.ubo.buffer;
    bufInfo.offset = 0;
    bufInfo.range  = sizeof(MaterialConstants);

    VkDescriptorImageInfo imgInfos[6];
    for (int i = 0; i < 6; ++i) {
        int texIdx = texIndices[i];
        if (mesh && texIdx >= 0 &&
            texIdx < static_cast<int>(mesh->textures().size())) {
            const auto& tex = mesh->textures()[texIdx].texture;
            imgInfos[i].imageView   = tex.view();
            imgInfos[i].sampler     = tex.sampler();
        } else {
            imgInfos[i].imageView   = m_fallback[i].view();
            imgInfos[i].sampler     = m_fallback[i].sampler();
        }
        imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkWriteDescriptorSet writes[7] = {};

    // Binding 0: UBO
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = ms.set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo     = &bufInfo;

    // Bindings 1–6: textures
    for (int i = 0; i < 6; ++i) {
        writes[i + 1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i + 1].dstSet          = ms.set;
        writes[i + 1].dstBinding      = static_cast<uint32_t>(i + 1);
        writes[i + 1].descriptorCount = 1;
        writes[i + 1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i + 1].pImageInfo      = &imgInfos[i];
    }

    vkUpdateDescriptorSets(device, 7, writes, 0, nullptr);

    return ms;
}

// ── createMaterialSet (MeshMaterial overload) ──────────────

SceneMaterialSet MaterialPipeline::createMaterialSet(
    VkCtx& ctx, const MeshMaterial& mat, const VkMesh& mesh)
{
    VkDevice device = ctx.device();
    SceneMaterialSet ms;

    ms.set = m_pool.allocate(device, m_layout);
    if (!ms.set) {
        SV_LOG_ERROR(TAG, "Failed to allocate material descriptor set (skinned)");
        return ms;
    }

    // Upload material constants from MeshMaterial
    MaterialConstants constants;
    constants.baseColor = mat.baseColor;
    constants.metallic  = mat.metallic;
    constants.roughness = mat.roughness;

    ms.ubo = VkBuf::createWithData(ctx, &constants, sizeof(constants),
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    // Map texture indices
    int texIndices[6] = {
        mat.baseColorTex,
        mat.normalTex,
        mat.metallicRoughnessTex,
        mat.emissiveTex,
        mat.occlusionTex,
        mat.opacityTex
    };

    // Build descriptor writes
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = ms.ubo.buffer;
    bufInfo.offset = 0;
    bufInfo.range  = sizeof(MaterialConstants);

    VkDescriptorImageInfo imgInfos[6];
    for (int i = 0; i < 6; ++i) {
        int texIdx = texIndices[i];
        if (texIdx >= 0 &&
            texIdx < static_cast<int>(mesh.textures().size())) {
            const auto& tex = mesh.textures()[texIdx].texture;
            imgInfos[i].imageView   = tex.view();
            imgInfos[i].sampler     = tex.sampler();
        } else {
            imgInfos[i].imageView   = m_fallback[i].view();
            imgInfos[i].sampler     = m_fallback[i].sampler();
        }
        imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkWriteDescriptorSet writes[7] = {};

    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = ms.set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo     = &bufInfo;

    for (int i = 0; i < 6; ++i) {
        writes[i + 1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i + 1].dstSet          = ms.set;
        writes[i + 1].dstBinding      = static_cast<uint32_t>(i + 1);
        writes[i + 1].descriptorCount = 1;
        writes[i + 1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i + 1].pImageInfo      = &imgInfos[i];
    }

    vkUpdateDescriptorSets(device, 7, writes, 0, nullptr);
    return ms;
}

// ── destroyMaterialSet ─────────────────────────────────────────────

void MaterialPipeline::destroyMaterialSet(VmaAllocator alloc,
                                          SceneMaterialSet& ms)
{
    ms.ubo.destroy(alloc);
    ms.set = VK_NULL_HANDLE;
}

// ── destroy ────────────────────────────────────────────────────────

void MaterialPipeline::destroy(VkCtx& ctx)
{
    VkDevice device       = ctx.device();
    VmaAllocator alloc    = ctx.allocator();

    for (auto& fb : m_fallback)
        fb.destroy(device, alloc);

    m_pool.destroy(device);

    if (m_layout) {
        vkDestroyDescriptorSetLayout(device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
}

} // namespace sv
