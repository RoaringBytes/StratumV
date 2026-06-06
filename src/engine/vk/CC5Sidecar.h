// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

namespace sv {

// Parsed CC5 JSON sidecar data for one material.
struct CC5MatInfo {
    std::string nodeType;     // "Hair", "Eyelash", "Brow", or empty
    std::string shaderName;   // "RLHair", "RLSkin", "RLEye", etc.
    bool twoSide    = false;
    bool hasOpacity = false;
    bool hasRootColor = false;
    glm::vec3 rootColor{1.0f};
    bool hasTipColor = false;
    glm::vec3 tipColor{1.0f};
    // Texture paths from JSON Textures section (relative to export dir)
    // Keys: "Base Color", "Normal", "Metallic", "Roughness", "Opacity", "AO"
    std::unordered_map<std::string, std::string> texturePaths;
};

using CC5MatMap = std::unordered_map<std::string, CC5MatInfo>;

// Parse the CC5 JSON sidecar file adjacent to an FBX file.
// Looks for <fbxPath without extension>.json. Returns empty map if not found.
CC5MatMap parseCC5Sidecar(const std::string& fbxPath);

} // namespace sv
