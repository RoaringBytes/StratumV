// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── AssetManifest ─────────────────────────────────────────
// JSON-driven asset preload list. See AssetManifest.h.

#include "AssetManifest.h"
#include "EngineLog.h"

#include <fstream>

namespace sv {

static constexpr const char* TAG = "AssetManifest";

// ── Enum <-> string conversion ─────────────────────────────────────

AssetKind parseAssetKind(const std::string& s)
{
    if (s == "mesh")      return AssetKind::Mesh;
    if (s == "texture")   return AssetKind::Texture;
    if (s == "audio")     return AssetKind::Audio;
    if (s == "shader")    return AssetKind::Shader;
    if (s == "scene")     return AssetKind::Scene;
    if (s == "material")  return AssetKind::Material;
    if (s == "animation") return AssetKind::Animation;
    return AssetKind::Other;
}

const char* assetKindToString(AssetKind k)
{
    switch (k) {
        case AssetKind::Mesh:      return "mesh";
        case AssetKind::Texture:   return "texture";
        case AssetKind::Audio:     return "audio";
        case AssetKind::Shader:    return "shader";
        case AssetKind::Scene:     return "scene";
        case AssetKind::Material:  return "material";
        case AssetKind::Animation: return "animation";
        case AssetKind::Other:     return "other";
    }
    return "other";
}

// ── Load / parse ───────────────────────────────────────────────────

bool AssetManifest::loadFromFile(const std::string& path)
{
    clear();
    m_filePath = path;

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        SV_LOG_ERROR(TAG, "Cannot open manifest: %s", path.c_str());
        return false;
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(ifs);
    } catch (const nlohmann::json::parse_error& e) {
        SV_LOG_ERROR(TAG, "Parse error in %s: %s", path.c_str(), e.what());
        return false;
    }

    if (!loadFromJson(doc)) {
        SV_LOG_ERROR(TAG, "Schema error in %s", path.c_str());
        return false;
    }

    SV_LOG_INFO(TAG, "Loaded manifest '%s' (%zu entries) from %s",
                m_name.c_str(), m_entries.size(), path.c_str());
    return true;
}

bool AssetManifest::loadFromJson(const nlohmann::json& doc)
{
    // Don't call clear() here — loadFromFile() already cleared and set
    // m_filePath. Tests that bypass loadFromFile() should call clear()
    // themselves if they need a pristine state.
    m_entries.clear();
    m_nameIndex.clear();
    m_name.clear();
    m_version = 0;

    // Version gate
    if (!doc.contains("version") || !doc["version"].is_number_integer()) {
        SV_LOG_ERROR(TAG, "Manifest missing integer 'version' field");
        return false;
    }
    int version = doc["version"].get<int>();
    if (version != 1) {
        SV_LOG_ERROR(TAG, "Unsupported manifest version %d (expected 1)",
                     version);
        return false;
    }
    m_version = version;

    // Optional name
    if (doc.contains("name") && doc["name"].is_string()) {
        m_name = doc["name"].get<std::string>();
    }

    // Assets array is optional — an empty manifest is valid.
    if (!doc.contains("assets")) {
        return true;
    }
    const auto& arr = doc["assets"];
    if (!arr.is_array()) {
        SV_LOG_ERROR(TAG, "'assets' must be an array");
        return false;
    }

    m_entries.reserve(arr.size());

    for (const auto& item : arr) {
        if (!item.is_object()) {
            SV_LOG_WARN(TAG, "Skipping non-object manifest entry");
            continue;
        }

        AssetEntry entry;
        // Required fields
        if (!item.contains("name") || !item["name"].is_string()) {
            SV_LOG_WARN(TAG, "Skipping manifest entry without 'name'");
            continue;
        }
        entry.name = item["name"].get<std::string>();

        if (!item.contains("path") || !item["path"].is_string()) {
            SV_LOG_WARN(TAG, "Skipping manifest entry '%s' without 'path'",
                        entry.name.c_str());
            continue;
        }
        entry.path = item["path"].get<std::string>();

        // Optional fields
        if (item.contains("kind") && item["kind"].is_string()) {
            entry.kindRaw = item["kind"].get<std::string>();
            entry.kind    = parseAssetKind(entry.kindRaw);
        }

        if (item.contains("preload") && item["preload"].is_boolean()) {
            entry.preload = item["preload"].get<bool>();
        }

        if (item.contains("tags") && item["tags"].is_object()) {
            entry.tags = item["tags"];
        }

        // Duplicate names: warn and keep the first occurrence. This
        // matches how SceneLoader handles duplicate object names.
        if (m_nameIndex.count(entry.name)) {
            SV_LOG_WARN(TAG, "Duplicate manifest entry name '%s' — ignoring",
                        entry.name.c_str());
            continue;
        }

        m_nameIndex[entry.name] = m_entries.size();
        m_entries.push_back(std::move(entry));
    }

    return true;
}

void AssetManifest::clear()
{
    m_entries.clear();
    m_nameIndex.clear();
    m_filePath.clear();
    m_name.clear();
    m_version = 0;
}

// ── Lookup ─────────────────────────────────────────────────────────

const AssetEntry* AssetManifest::find(const std::string& name) const
{
    auto it = m_nameIndex.find(name);
    if (it == m_nameIndex.end()) return nullptr;
    return &m_entries[it->second];
}

bool AssetManifest::contains(const std::string& name) const
{
    return m_nameIndex.find(name) != m_nameIndex.end();
}

std::vector<const AssetEntry*>
AssetManifest::entriesOfKind(AssetKind k) const
{
    std::vector<const AssetEntry*> out;
    out.reserve(m_entries.size());
    for (const auto& e : m_entries) {
        if (e.kind == k) out.push_back(&e);
    }
    return out;
}

std::vector<const AssetEntry*>
AssetManifest::preloadEntries() const
{
    std::vector<const AssetEntry*> out;
    out.reserve(m_entries.size());
    for (const auto& e : m_entries) {
        if (e.preload) out.push_back(&e);
    }
    return out;
}

} // namespace sv
