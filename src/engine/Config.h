// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <nlohmann/json.hpp>
#include <glm/vec3.hpp>
#include <string>
#include <functional>
#include <cstdint>

namespace sv {

class AssetWatcher;

// ── World bounds ───────────────────────────────────────────────────
// Engine-wide playable-area AABB. Games use this to clamp cameras,
// cull distant terrain, and bound physics queries. Defaults are
// intentionally huge so unset bounds never clip legitimate content.
struct WorldBounds {
    glm::vec3 min{-10000.0f, -10000.0f, -10000.0f};
    glm::vec3 max{ 10000.0f,  10000.0f,  10000.0f};

    // True iff min < max on all three axes.
    bool valid() const {
        return min.x < max.x && min.y < max.y && min.z < max.z;
    }

    // World-space size on each axis (max - min). Negative if invalid.
    glm::vec3 size() const { return max - min; }

    // Center of the AABB.
    glm::vec3 center() const { return (min + max) * 0.5f; }

    // Point-in-bounds test (inclusive).
    bool contains(const glm::vec3& p) const {
        return p.x >= min.x && p.x <= max.x
            && p.y >= min.y && p.y <= max.y
            && p.z >= min.z && p.z <= max.z;
    }
};

class Config {
public:
    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path) const;
    void loadDefaults();

    // Dot-path accessor: config.get<int>("window.width", 1920)
    template<typename T>
    T get(const std::string& dotPath, const T& defaultVal) const;

    template<typename T>
    void set(const std::string& dotPath, const T& value);

    // ── glm::vec3 helpers (stored as JSON arrays [x, y, z]) ────────
    // Reads a 3-element JSON array at the given dot path. Falls back
    // to `defaultVal` if the path is missing, not an array, or has
    // fewer than 3 numeric elements.
    glm::vec3 getVec3(const std::string& dotPath,
                      const glm::vec3& defaultVal) const;

    // Writes a glm::vec3 as a 3-element JSON array at the dot path.
    void setVec3(const std::string& dotPath, const glm::vec3& v);

    // ── World bounds ───────────────────────────────────────────────
    // Convenience accessors for "world.boundsMin" / "world.boundsMax".
    // Defaults to WorldBounds{} (i.e. ±10000 on each axis) if the
    // config doesn't specify a world section.
    WorldBounds worldBounds() const;
    void        setWorldBounds(const WorldBounds& b);

    // Returns true if file changed on disk since last load/check
    bool checkReload();

    // Register config file with an AssetWatcher for automatic hot-reload.
    // Optional callback fires after successful reload with the new JSON data.
    void enableAutoReload(AssetWatcher& watcher, std::function<void(const Config&)> onReload = nullptr);

    const nlohmann::json& raw() const { return m_data; }
    const std::string& path() const { return m_path; }

private:
    // Navigate to parent object and extract leaf key from dot path
    const nlohmann::json* resolve(const std::string& dotPath, std::string& leafKey) const;
    nlohmann::json* resolveMut(const std::string& dotPath, std::string& leafKey);

    nlohmann::json m_data;
    std::string    m_path;
    int64_t        m_lastModified = 0;

    static int64_t getFileTimestamp(const std::string& path);
};

// ── Template implementations ────────────────────────────────────

template<typename T>
T Config::get(const std::string& dotPath, const T& defaultVal) const
{
    std::string key;
    const nlohmann::json* parent = resolve(dotPath, key);
    if (!parent || !parent->contains(key)) return defaultVal;
    try {
        return (*parent)[key].get<T>();
    } catch (...) {
        return defaultVal;
    }
}

template<typename T>
void Config::set(const std::string& dotPath, const T& value)
{
    std::string key;
    nlohmann::json* parent = resolveMut(dotPath, key);
    if (parent) {
        (*parent)[key] = value;
    }
}

} // namespace sv
