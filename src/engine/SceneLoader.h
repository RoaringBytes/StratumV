// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── SceneLoader ────────────────────────────────────────────────────
// Loads .scene.json files exported by the StratumV Blender addon.
// Produces positioned VkMesh instances with a parent/child transform
// hierarchy.  Supports AssetWatcher hot-reload.
// Layer 5 — depends on: VkMesh (L1), AssetWatcher (L3), EngineLog (L4)

#include "vk/VkMesh.h"
#include "MaterialPipeline.h"
#include "FrustumCuller.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace sv {

class VkCtx;
class AssetWatcher;

// ── Scene node — one mesh instance with transform ──────────────────
struct SceneNode {
    std::string name;
    std::string meshPath;        // relative .glb path from scene file

    // Local transform (from JSON)
    glm::vec3   position{0.0f};
    glm::quat   rotation{1.0f, 0.0f, 0.0f, 0.0f}; // w,x,y,z
    glm::vec3   localScale{1.0f};

    // Computed world-space transform
    glm::mat4   worldTransform{1.0f};

    // Hierarchy (indices into SceneLoader::nodes(); -1 = root)
    int         parent   = -1;
    std::vector<int> children;

    // Material overrides from scene file
    glm::vec4   baseColor{1, 1, 1, 1};
    float       metallic  = 0.0f;
    float       roughness = 1.0f;

    // World-space AABB (from JSON, already in engine coords)
    glm::vec3   boundsMin{0.0f};
    glm::vec3   boundsMax{0.0f};

    // Custom properties forwarded from Blender
    nlohmann::json customProperties;

    // Index into SceneLoader's shared mesh array (-1 = not loaded)
    int         meshIndex = -1;

    // Local TRS → mat4
    glm::mat4 localMatrix() const {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
        m *= glm::mat4_cast(rotation);
        m = glm::scale(m, localScale);
        return m;
    }
};

// ── Scene marker — empty with type + properties ────────────────────
struct SceneMarker {
    std::string    name;
    std::string    type;        // spawn_point, trigger, audio_zone, etc.
    glm::vec3      position{0.0f};
    glm::quat      rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3      scale{1.0f};
    nlohmann::json properties;

    glm::mat4 matrix() const {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
        m *= glm::mat4_cast(rotation);
        m = glm::scale(m, scale);
        return m;
    }
};

// ── Marker filtering ───────────────────────────────────────────────
// Free-function marker filter so tests can exercise the logic without
// needing a live VkCtx to drive SceneLoader::loadFromFile().
// Returns pointers into the supplied vector — valid for that vector's
// lifetime. Relative order is preserved.
std::vector<const SceneMarker*>
filterMarkersByType(const std::vector<SceneMarker>& markers,
                    const std::string& type);

// ── SceneLoader ────────────────────────────────────────────────────
class SceneLoader {
public:
    // Load a .scene.json file.  Resolves mesh paths relative to the
    // scene file's directory.  Returns true on success.
    bool loadFromFile(VkCtx& ctx, const std::string& path,
                      bool loadTextures = false);

    // Reload from the same path (hot-reload entry point).
    bool reload();

    // Register this scene file for hot-reload via AssetWatcher.
    void enableHotReload(AssetWatcher& watcher);

    // Build per-node material descriptor sets via the MaterialPipeline.
    // Call after loadFromFile().  Nodes without meshes get null sets.
    void buildMaterials(MaterialPipeline& pipeline);

    // Destroy all loaded GPU resources (meshes + material sets).
    void destroy(VmaAllocator alloc);

    // ── Frustum culling ──
    // Returns indices of nodes whose world-space AABB is at least
    // partially inside the frustum defined by the view-projection matrix.
    // Nodes without bounds (boundsMin == boundsMax) are always included.
    std::vector<int> cullVisible(const glm::mat4& viewProj) const;

    // ── Spawn points / marker queries ──
    // Returns all markers whose `type` string matches. Pass the default
    // ("spawn_point") for player/NPC spawn locations, or any other type
    // the scene file uses ("trigger", "audio_zone", "waypoint", etc.).
    // Returned pointers alias m_markers — valid until the scene reloads.
    std::vector<const SceneMarker*>
    getSpawnPoints(const std::string& type = "spawn_point") const;

    // ── Accessors ──
    const std::vector<SceneNode>&   nodes()   const { return m_nodes; }
    const std::vector<SceneMarker>& markers() const { return m_markers; }

    // Find node by name.  Returns nullptr if not found.
    const SceneNode* findNode(const std::string& name) const;

    // Scene metadata
    const std::string& sceneName()  const { return m_sceneName; }
    const std::string& filePath()   const { return m_filePath; }
    int                version()    const { return m_version; }

    // Shared meshes (deduplicated by path)
    const std::vector<std::shared_ptr<VkMesh>>& meshes() const { return m_meshes; }

    // Per-node material sets (parallel to nodes(); null set for nodes without meshes)
    const std::vector<SceneMaterialSet>& materialSets() const { return m_materialSets; }

private:
    void parseTransform(const nlohmann::json& j,
                        glm::vec3& pos, glm::quat& rot, glm::vec3& scl);
    void resolveHierarchy();
    void computeWorldTransforms();
    void computeWorldTransformRecursive(int idx);
    int  loadOrShareMesh(VkCtx& ctx, const std::string& absPath,
                         bool loadTextures);

    std::vector<SceneNode>               m_nodes;
    std::vector<SceneMarker>             m_markers;
    std::vector<std::shared_ptr<VkMesh>> m_meshes;
    std::vector<SceneMaterialSet>        m_materialSets;
    std::unordered_map<std::string, int> m_meshPathToIndex;

    // Name → node index for hierarchy resolution
    std::unordered_map<std::string, int> m_nameToIndex;

    std::string m_filePath;
    std::string m_baseDir;     // directory containing the scene file
    std::string m_sceneName;
    int         m_version = 0;

    // Stored for reload
    VkCtx*      m_ctx          = nullptr;
    bool        m_loadTextures = false;
};

} // namespace sv
