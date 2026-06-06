// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "VkMesh.h"   // MeshVertex, SubMesh, MeshMaterial, SkeletonData, TextureType
#include <vector>
#include <string>
#include <cstdint>

namespace sv {

// CPU-side decoded texture ready for GPU upload.
struct TextureImportData {
    std::vector<uint8_t> pixels;  // RGBA8
    uint32_t width  = 0;
    uint32_t height = 0;
    TextureType type = TextureType::baseColor;
    bool srgb = true;
};

// Intermediate representation produced by format-specific loaders.
// VkMesh consumes this to build GPU resources.
struct MeshImportData {
    std::vector<MeshVertex>       vertices;
    std::vector<uint32_t>         indices;
    std::vector<SubMesh>          submeshes;
    std::vector<MeshMaterial>     materials;
    std::vector<TextureImportData> textures;
    SkeletonData                  skeleton;

    // Morph target CPU data (packed into SSBO by VkMesh)
    size_t morphTargetCount = 0;
    std::vector<std::vector<glm::vec3>> morphPosDeltas;   // [target][vertex]
    std::vector<std::vector<glm::vec3>> morphNormDeltas;  // [target][vertex]
    std::vector<std::string>            morphTargetNames;
    std::vector<float>                  morphTargetDefaultWeights;
};

} // namespace sv
