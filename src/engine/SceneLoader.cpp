// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── SceneLoader ─────────────────────────────────────────────
// Layer 5 — loads .scene.json → positioned VkMesh instances

#include "SceneLoader.h"
#include "AssetWatcher.h"
#include "EngineLog.h"
#include "vk/VkContext.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace sv {

static constexpr const char* TAG = "SceneLoader";

// ── JSON helpers ───────────────────────────────────────────────────

static glm::vec3 readVec3(const json& j, glm::vec3 fallback = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() < 3) return fallback;
    return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
}

static glm::vec4 readVec4(const json& j, glm::vec4 fallback = glm::vec4(1.0f)) {
    if (!j.is_array() || j.size() < 4) return fallback;
    return { j[0].get<float>(), j[1].get<float>(),
             j[2].get<float>(), j[3].get<float>() };
}

// Schema stores quaternion as [x, y, z, w]; glm::quat ctor is (w, x, y, z).
static glm::quat readQuat(const json& j) {
    if (!j.is_array() || j.size() < 4)
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    float x = j[0].get<float>();
    float y = j[1].get<float>();
    float z = j[2].get<float>();
    float w = j[3].get<float>();
    return glm::quat(w, x, y, z);
}

// ── SceneLoader ────────────────────────────────────────────────────

bool SceneLoader::loadFromFile(VkCtx& ctx, const std::string& path,
                               bool loadTextures)
{
    m_ctx          = &ctx;
    m_loadTextures = loadTextures;
    m_filePath     = path;
    m_baseDir      = fs::path(path).parent_path().string();

    // Read and parse JSON
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        SV_LOG_ERROR(TAG, "Cannot open scene file: %s", path.c_str());
        return false;
    }

    json doc;
    try {
        doc = json::parse(ifs);
    } catch (const json::parse_error& e) {
        SV_LOG_ERROR(TAG, "JSON parse error in %s: %s", path.c_str(), e.what());
        return false;
    }
    ifs.close();

    // Validate version
    if (!doc.contains("version") || doc["version"].get<int>() != 1) {
        SV_LOG_ERROR(TAG, "Unsupported scene version in %s (expected 1)",
                     path.c_str());
        return false;
    }
    m_version = 1;

    // Metadata
    if (doc.contains("metadata") && doc["metadata"].contains("scene_name"))
        m_sceneName = doc["metadata"]["scene_name"].get<std::string>();

    // ── Parse objects ──────────────────────────────────────────────
    if (!doc.contains("objects") || !doc["objects"].is_array()) {
        SV_LOG_WARN(TAG, "No objects array in %s", path.c_str());
        return true; // valid but empty scene
    }

    const auto& objects = doc["objects"];
    m_nodes.reserve(objects.size());
    m_nameToIndex.clear();

    for (size_t i = 0; i < objects.size(); ++i) {
        const auto& obj = objects[i];
        SceneNode node;

        node.name = obj.value("name", "unnamed_" + std::to_string(i));

        // Mesh path — resolve relative to scene file directory
        if (obj.contains("mesh"))
            node.meshPath = obj["mesh"].get<std::string>();

        // Transform
        if (obj.contains("transform"))
            parseTransform(obj["transform"], node.position,
                           node.rotation, node.localScale);

        // Material overrides
        if (obj.contains("material")) {
            const auto& mat = obj["material"];
            if (mat.contains("baseColor"))
                node.baseColor = readVec4(mat["baseColor"]);
            node.metallic  = mat.value("metallic",  0.0f);
            node.roughness = mat.value("roughness", 1.0f);
        }

        // Bounds
        if (obj.contains("bounds")) {
            const auto& b = obj["bounds"];
            if (b.contains("min")) node.boundsMin = readVec3(b["min"]);
            if (b.contains("max")) node.boundsMax = readVec3(b["max"]);
        }

        // Custom properties
        if (obj.contains("custom_properties"))
            node.customProperties = obj["custom_properties"];

        m_nameToIndex[node.name] = static_cast<int>(m_nodes.size());
        m_nodes.push_back(std::move(node));
    }

    // ── Resolve parent/child hierarchy ─────────────────────────────
    // First pass: set parent indices from "parent" name strings
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto& obj = objects[i];
        if (obj.contains("parent") && !obj["parent"].is_null()) {
            std::string parentName = obj["parent"].get<std::string>();
            auto it = m_nameToIndex.find(parentName);
            if (it != m_nameToIndex.end()) {
                m_nodes[i].parent = it->second;
            } else {
                SV_LOG_WARN(TAG, "Object '%s' references unknown parent '%s'",
                            m_nodes[i].name.c_str(), parentName.c_str());
            }
        }
    }
    resolveHierarchy();

    // ── Compute world transforms ───────────────────────────────────
    computeWorldTransforms();

    // ── Load meshes (deduplicated by path) ─────────────────────────
    for (auto& node : m_nodes) {
        if (node.meshPath.empty()) continue;

        std::string absPath = (fs::path(m_baseDir) / node.meshPath).string();
        node.meshIndex = loadOrShareMesh(ctx, absPath, loadTextures);
    }

    // ── Parse markers ──────────────────────────────────────────────
    if (doc.contains("markers") && doc["markers"].is_array()) {
        const auto& markers = doc["markers"];
        m_markers.reserve(markers.size());

        for (const auto& mk : markers) {
            SceneMarker marker;
            marker.name = mk.value("name", "unnamed_marker");
            marker.type = mk.value("type", "custom");

            if (mk.contains("transform"))
                parseTransform(mk["transform"], marker.position,
                               marker.rotation, marker.scale);

            if (mk.contains("properties"))
                marker.properties = mk["properties"];

            m_markers.push_back(std::move(marker));
        }
    }

    SV_LOG_INFO(TAG, "Loaded scene '%s': %d objects, %d unique meshes, %d markers",
                m_sceneName.c_str(), (int)m_nodes.size(),
                (int)m_meshes.size(), (int)m_markers.size());
    return true;
}

void SceneLoader::buildMaterials(MaterialPipeline& pipeline)
{
    if (!m_ctx) return;

    // Destroy existing material sets
    VmaAllocator alloc = m_ctx->allocator();
    for (auto& ms : m_materialSets)
        pipeline.destroyMaterialSet(alloc, ms);
    m_materialSets.clear();

    m_materialSets.resize(m_nodes.size());

    int created = 0;
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        const auto& node = m_nodes[i];
        if (node.meshIndex < 0) continue;

        const VkMesh* mesh = m_meshes[node.meshIndex].get();
        m_materialSets[i] = pipeline.createMaterialSet(*m_ctx, node, mesh);
        ++created;
    }

    SV_LOG_INFO(TAG, "Built %d material sets for '%s'",
                created, m_sceneName.c_str());
}

bool SceneLoader::reload()
{
    if (!m_ctx || m_filePath.empty()) {
        SV_LOG_ERROR(TAG, "Cannot reload — no scene loaded");
        return false;
    }

    SV_LOG_INFO(TAG, "Hot-reloading scene: %s", m_filePath.c_str());

    // Preserve allocator for cleanup
    VmaAllocator alloc = m_ctx->allocator();

    // Destroy current GPU resources
    destroy(alloc);

    // Reload
    return loadFromFile(*m_ctx, m_filePath, m_loadTextures);
}

void SceneLoader::enableHotReload(AssetWatcher& watcher)
{
    if (m_filePath.empty()) return;

    watcher.watch(m_filePath, [this]() {
        reload();
    });

    SV_LOG_INFO(TAG, "Hot-reload enabled for: %s", m_filePath.c_str());
}

void SceneLoader::destroy(VmaAllocator alloc)
{
    // Destroy material UBOs (descriptor sets freed with pool)
    for (auto& ms : m_materialSets)
        ms.ubo.destroy(alloc);
    m_materialSets.clear();

    for (auto& mesh : m_meshes) {
        if (mesh) mesh->destroy(alloc);
    }
    m_meshes.clear();
    m_meshPathToIndex.clear();
    m_nodes.clear();
    m_markers.clear();
    m_nameToIndex.clear();
    m_sceneName.clear();
    m_version = 0;
}

std::vector<int> SceneLoader::cullVisible(const glm::mat4& viewProj) const
{
    FrustumCuller frustum;
    frustum.extractFromMatrix(viewProj);

    std::vector<int> visible;
    visible.reserve(m_nodes.size());

    for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i) {
        const auto& node = m_nodes[i];

        // Skip nodes without a mesh
        if (node.meshIndex < 0) continue;

        // Degenerate bounds (zero-size) — always include
        if (node.boundsMin == node.boundsMax) {
            visible.push_back(i);
            continue;
        }

        // Transform the AABB corners by the world transform to get
        // a world-space AABB.  Since worldTransform can include rotation,
        // we recompute a tight axis-aligned bounding box.
        const glm::mat4& w = node.worldTransform;
        const glm::vec3& lo = node.boundsMin;
        const glm::vec3& hi = node.boundsMax;

        // Arvo's method: compute world-space AABB from local AABB + matrix
        glm::vec3 center = (lo + hi) * 0.5f;
        glm::vec3 extent = (hi - lo) * 0.5f;

        glm::vec3 wCenter = glm::vec3(w * glm::vec4(center, 1.0f));

        // For each axis, accumulate the absolute contribution of each
        // matrix column scaled by the extent
        glm::vec3 wExtent(0.0f);
        for (int c = 0; c < 3; ++c) {
            wExtent.x += std::abs(w[c][0]) * extent[c];
            wExtent.y += std::abs(w[c][1]) * extent[c];
            wExtent.z += std::abs(w[c][2]) * extent[c];
        }

        glm::vec3 wMin = wCenter - wExtent;
        glm::vec3 wMax = wCenter + wExtent;

        if (frustum.isVisible(wMin, wMax))
            visible.push_back(i);
    }

    return visible;
}

const SceneNode* SceneLoader::findNode(const std::string& name) const
{
    auto it = m_nameToIndex.find(name);
    if (it == m_nameToIndex.end()) return nullptr;
    return &m_nodes[it->second];
}

// ── Marker filtering ─────────────────────────────────────

std::vector<const SceneMarker*>
filterMarkersByType(const std::vector<SceneMarker>& markers,
                    const std::string& type)
{
    std::vector<const SceneMarker*> result;
    result.reserve(markers.size());
    for (const auto& m : markers) {
        if (m.type == type) result.push_back(&m);
    }
    return result;
}

std::vector<const SceneMarker*>
SceneLoader::getSpawnPoints(const std::string& type) const
{
    return filterMarkersByType(m_markers, type);
}

// ── Private helpers ────────────────────────────────────────────────

void SceneLoader::parseTransform(const json& j,
                                  glm::vec3& pos, glm::quat& rot,
                                  glm::vec3& scl)
{
    if (j.contains("position")) pos = readVec3(j["position"]);
    if (j.contains("rotation")) rot = readQuat(j["rotation"]);
    if (j.contains("scale"))    scl = readVec3(j["scale"], glm::vec3(1.0f));
}

void SceneLoader::resolveHierarchy()
{
    // Clear children arrays, then rebuild from parent indices
    for (auto& node : m_nodes) node.children.clear();

    for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i) {
        int p = m_nodes[i].parent;
        if (p >= 0 && p < static_cast<int>(m_nodes.size()))
            m_nodes[p].children.push_back(i);
    }
}

void SceneLoader::computeWorldTransforms()
{
    // Mark all as unvisited (identity)
    for (auto& node : m_nodes)
        node.worldTransform = glm::mat4(0.0f); // sentinel

    for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
        computeWorldTransformRecursive(i);
}

void SceneLoader::computeWorldTransformRecursive(int idx)
{
    auto& node = m_nodes[idx];

    // Already computed?
    if (node.worldTransform != glm::mat4(0.0f)) return;

    glm::mat4 local = node.localMatrix();

    if (node.parent >= 0) {
        computeWorldTransformRecursive(node.parent);
        node.worldTransform = m_nodes[node.parent].worldTransform * local;
    } else {
        node.worldTransform = local;
    }
}

int SceneLoader::loadOrShareMesh(VkCtx& ctx, const std::string& absPath,
                                  bool loadTextures)
{
    auto it = m_meshPathToIndex.find(absPath);
    if (it != m_meshPathToIndex.end()) return it->second;

    auto mesh = std::make_shared<VkMesh>();
    if (!mesh->loadFromFile(ctx, absPath, loadTextures)) {
        SV_LOG_WARN(TAG, "Failed to load mesh: %s", absPath.c_str());
        return -1;
    }

    int index = static_cast<int>(m_meshes.size());
    m_meshes.push_back(std::move(mesh));
    m_meshPathToIndex[absPath] = index;
    return index;
}

} // namespace sv
