// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "VkBuffer.h"
#include "VkTexture.h"
#include "../MorphTargetTypes.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <string>
#include <cstdint>
#include <memory>

namespace sv {

class VkCtx;

struct MeshVertex {
    glm::vec3  pos;
    glm::vec3  normal;
    glm::vec2  uv;
    glm::uvec4 joints{0};   // bone indices (4 influences)
    glm::vec4  weights{0};  // bone weights (4 influences)
};

struct SubMesh {
    uint32_t indexOffset  = 0;
    uint32_t indexCount   = 0;
    int      materialIndex = -1;
};

// Texture type tag — identifies how a texture is used in a PBR material.
enum class TextureType : uint8_t {
    baseColor,
    normal,
    metallicRoughness,
    emissive,
    occlusion,
    opacity
};

// GPU texture extracted from a glTF model.
struct MeshTexture {
    VkTex       texture;
    TextureType type = TextureType::baseColor;
};

// Skeleton joint — one entry per bone in the hierarchy.
struct SkeletonJoint {
    std::string name;
    int         parent = -1;             // index into SkeletonData::joints (-1 = root)
    glm::mat4   inverseBindMatrix{1.0f}; // inverse bind pose transform
    // Rest pose (node-local transform from glTF)
    glm::vec3   restTranslation{0.0f};
    glm::quat   restRotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity (w,x,y,z)
    glm::vec3   restScale{1.0f};
};

// Parsed skin/skeleton data from a glTF file.
struct SkeletonData {
    std::vector<SkeletonJoint> joints;

    int jointCount() const { return (int)joints.size(); }
    bool empty()     const { return joints.empty(); }
};

// ── Axis-aligned bounding box ─────────────────────────────────────
// Local-space AABB computed from a mesh's vertex positions. Used
// by consumers that need to frame a mesh in a camera (thumbnail
// bake, scene preview, quick culling). Defaults to an empty
// (invalid) box so uninitialised values can be detected.
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    bool      valid = false;

    glm::vec3 center()  const { return (min + max) * 0.5f; }
    glm::vec3 size()    const { return max - min; }
    glm::vec3 extents() const { return (max - min) * 0.5f; }

    // Longest axis length of the box. 0 for invalid/zero-sized.
    // Intentionally written without std::max so the header stays
    // free of <algorithm> and the NOMINMAX/windows.h macro dance.
    float longestAxis() const {
        const glm::vec3 s = size();
        float m = s.x;
        if (s.y > m) m = s.y;
        if (s.z > m) m = s.z;
        return m;
    }

    // Bounding-sphere radius centred at center().
    float radius() const { return glm::length(extents()); }
};

// Free function: compute the AABB of a vector of mesh vertices.
// Returns `{ .valid = false }` for an empty input. Pure CPU math —
// unit-testable without a GPU context.
AABB computeMeshAABB(const std::vector<MeshVertex>& vertices);

// Blend mode for skinned mesh materials.
enum class BlendMode : uint8_t { Opaque, AlphaBlend };

struct MeshMaterial {
    glm::vec4 baseColor{1, 1, 1, 1};
    float     metallic  = 0.0f;
    float     roughness = 1.0f;
    // Indices into textures() array; -1 = no texture
    int       baseColorTex         = -1;
    int       normalTex            = -1;
    int       metallicRoughnessTex = -1;
    int       emissiveTex          = -1;
    int       occlusionTex         = -1;
    int       opacityTex           = -1;
    BlendMode blendMode = BlendMode::Opaque;
    bool      twoSided  = false;
    std::string name;   // source material name (for debug / auto-detection)
};

class VkMesh {
public:
    bool loadFromFile(VkCtx& ctx, const std::string& path, bool loadTextures = false);
    void destroy(VmaAllocator alloc);

    VkBuffer vertexBuffer() const { return m_vbo.buffer; }
    VkBuffer indexBuffer()  const { return m_ibo.buffer; }

    const std::vector<SubMesh>&      submeshes() const { return m_submeshes; }
    const std::vector<MeshMaterial>& materials() const  { return m_materials; }
    const std::vector<MeshTexture>&  textures()  const { return m_textures; }
    uint32_t totalIndexCount() const { return m_totalIndexCount; }

    bool                 isSkinned() const { return !m_skeleton.empty(); }
    const SkeletonData&  skeleton()  const { return m_skeleton; }

    bool                    hasMorphTargets() const { return m_morphTargets.hasMorphTargets(); }
    const MorphTargetData&  morphTargets()    const { return m_morphTargets; }
    MorphTargetData&        morphTargets()          { return m_morphTargets; }
    uint32_t                vertexCount()     const { return m_vertexCount; }

    // Local-space AABB, computed from vertex positions during
    // loadFromFile(). Returns an invalid AABB (`valid == false`)
    // if the mesh has not been loaded yet.
    const AABB&             aabb() const { return m_aabb; }

private:
    VkBuf m_vbo{}, m_ibo{};
    std::vector<SubMesh>      m_submeshes;
    std::vector<MeshMaterial> m_materials;
    std::vector<MeshTexture>  m_textures;
    SkeletonData              m_skeleton;
    MorphTargetData           m_morphTargets;
    AABB                      m_aabb{};
    uint32_t m_totalIndexCount = 0;
    uint32_t m_vertexCount     = 0;
};

} // namespace sv
