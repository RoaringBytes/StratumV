// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#ifdef _WIN32
#define NOMINMAX
#endif

#include "VkMesh.h"
#include "VkContext.h"
#include "MeshImportData.h"
#include "GltfLoader.h"
#include "FbxLoader.h"

#include <cstdio>
#include <algorithm>
#include <string>

namespace sv {

// ── Local-space AABB computation ─────────────────────────────────────
// Free function so unit tests can exercise the math without a VkCtx.
// An empty vertex list yields a default-constructed (invalid) AABB.
AABB computeMeshAABB(const std::vector<MeshVertex>& vertices)
{
    AABB a{};
    if (vertices.empty()) return a;
    a.min = vertices[0].pos;
    a.max = vertices[0].pos;
    for (size_t i = 1; i < vertices.size(); ++i) {
        const glm::vec3& p = vertices[i].pos;
        if (p.x < a.min.x) a.min.x = p.x;
        if (p.y < a.min.y) a.min.y = p.y;
        if (p.z < a.min.z) a.min.z = p.z;
        if (p.x > a.max.x) a.max.x = p.x;
        if (p.y > a.max.y) a.max.y = p.y;
        if (p.z > a.max.z) a.max.z = p.z;
    }
    a.valid = true;
    return a;
}

bool VkMesh::loadFromFile(VkCtx& ctx, const std::string& path, bool loadTextures)
{
    // ── Route by file extension ─────────────────────────────────────────
    MeshImportData data;
    bool ok = false;
    {
        auto dot = path.find_last_of('.');
        std::string ext;
        if (dot != std::string::npos) {
            ext = path.substr(dot);
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        }
        if (ext == ".fbx") {
            ok = loadFbx(path, loadTextures, data);
        } else {
            ok = loadGltf(path, loadTextures, data);
        }
    }

    if (!ok) return false;
    if (data.vertices.empty()) {
        printf("[VkMesh] No geometry loaded from: %s\n", path.c_str());
        return false;
    }

    // ── Compute local-space AABB before the vertex vector moves ────────
    // Must happen BEFORE we hand `data.vertices` to VkBuf::createWithData
    // because the upload only reads the raw pointer + byte size — the
    // vector itself stays intact. But running the walk here keeps the
    // invariant "aabb is set whenever the mesh is loaded" obvious.
    m_aabb = computeMeshAABB(data.vertices);

    // ── Move parsed CPU data to members ─────────────────────────────────
    m_submeshes    = std::move(data.submeshes);
    m_materials    = std::move(data.materials);
    m_skeleton     = std::move(data.skeleton);
    m_vertexCount  = (uint32_t)data.vertices.size();

    // ── Upload geometry to GPU ──────────────────────────────────────────
    m_vbo = VkBuf::createWithData(ctx, data.vertices.data(),
        data.vertices.size() * sizeof(MeshVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_ibo = VkBuf::createWithData(ctx, data.indices.data(),
        data.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    m_totalIndexCount = (uint32_t)data.indices.size();

    // ── Upload textures to GPU ──────────────────────────────────────────
    if (!data.textures.empty()) {
        m_textures.reserve(data.textures.size());
        for (auto& tex : data.textures) {
            MeshTexture mt;
            mt.type = tex.type;
            if (tex.pixels.empty() || tex.width == 0 || tex.height == 0) {
                // Invalid — push placeholder to preserve material indices
                m_textures.push_back(std::move(mt));
                continue;
            }
            if (!mt.texture.loadFromMemory(ctx, tex.pixels.data(), tex.width, tex.height, tex.srgb)) {
                printf("[VkMesh] Warning: texture upload failed (%ux%u)\n", tex.width, tex.height);
            }
            m_textures.push_back(std::move(mt));
        }
    }

    // ── Pack morph target SSBO ──────────────────────────────────────────
    if (data.morphTargetCount > 0) {
        for (size_t t = 0; t < data.morphTargetCount; t++) {
            data.morphPosDeltas[t].resize(m_vertexCount, glm::vec3(0.0f));
            data.morphNormDeltas[t].resize(m_vertexCount, glm::vec3(0.0f));
        }
        size_t totalPairs = data.morphTargetCount * m_vertexCount * 2;
        std::vector<glm::vec4> ssboData(totalPairs, glm::vec4(0.0f));
        for (size_t t = 0; t < data.morphTargetCount; t++) {
            for (size_t v = 0; v < m_vertexCount; v++) {
                size_t idx = (t * m_vertexCount + v) * 2;
                ssboData[idx + 0] = glm::vec4(data.morphPosDeltas[t][v], 0.0f);
                ssboData[idx + 1] = glm::vec4(data.morphNormDeltas[t][v], 0.0f);
            }
        }
        m_morphTargets.deltaSSBO = VkBuf::createWithData(ctx, ssboData.data(),
            ssboData.size() * sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        m_morphTargets.vertexCount = m_vertexCount;
        m_morphTargets.targets.resize(data.morphTargetCount);

        for (size_t t = 0; t < data.morphTargetCount; t++) {
            if (t < data.morphTargetNames.size())
                m_morphTargets.targets[t].name = std::move(data.morphTargetNames[t]);
            if (m_morphTargets.targets[t].name.empty())
                m_morphTargets.targets[t].name = "target_" + std::to_string(t);
            if (t < data.morphTargetDefaultWeights.size())
                m_morphTargets.targets[t].defaultWeight = data.morphTargetDefaultWeights[t];
        }

        printf("[VkMesh] Morph targets: %d targets, %d verts (%.1f MB SSBO) from '%s'\n",
               (int)data.morphTargetCount, m_vertexCount,
               (float)(totalPairs * sizeof(glm::vec4)) / (1024.0f * 1024.0f), path.c_str());
    }

    printf("[VkMesh] Loaded '%s': %d verts, %d indices, %d submeshes, %d materials\n",
           path.c_str(), (int)data.vertices.size(), (int)data.indices.size(),
           (int)m_submeshes.size(), (int)m_materials.size());

    return true;
}

void VkMesh::destroy(VmaAllocator alloc)
{
    m_vbo.destroy(alloc);
    m_ibo.destroy(alloc);
    // VkTex::destroy needs VkDevice — retrieve from VMA allocator info
    VmaAllocatorInfo allocInfo{};
    vmaGetAllocatorInfo(alloc, &allocInfo);
    m_morphTargets.destroy(allocInfo.device, alloc);
    for (auto& mt : m_textures)
        mt.texture.destroy(allocInfo.device, alloc);
    m_textures.clear();
    m_submeshes.clear();
    m_materials.clear();
    m_skeleton.joints.clear();
    m_aabb = AABB{};
    m_totalIndexCount = 0;
    m_vertexCount = 0;
}

} // namespace sv
