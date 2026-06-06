// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "Config.h"
#include "AssetWatcher.h"
#include "EngineLog.h"

#include <filesystem>
#include <fstream>
#include <cstdio>

namespace sv {

bool Config::loadFromFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        SV_LOG_WARN("Config", "Could not open %s, using defaults", path.c_str());
        loadDefaults();
        return false;
    }

    try {
        m_data = nlohmann::json::parse(file);
    } catch (const std::exception& e) {
        SV_LOG_ERROR("Config", "Parse error in %s: %s", path.c_str(), e.what());
        loadDefaults();
        return false;
    }

    m_path = path;
    m_lastModified = getFileTimestamp(path);
    SV_LOG_INFO("Config", "Loaded: %s", path.c_str());
    return true;
}

bool Config::saveToFile(const std::string& path) const
{
    std::ofstream file(path);
    if (!file.is_open()) {
        printf("[StratumV] Config: could not write %s\n", path.c_str());
        return false;
    }

    file << m_data.dump(4);
    return file.good();
}

void Config::loadDefaults()
{
    m_data = nlohmann::json{
        {"window", {
            {"width", 1920},
            {"height", 1080},
            {"title", "StratumV [Vulkan]"},
            {"vsync", false}
        }},
        {"camera", {
            {"startX", 0.0f},
            {"startY", 40.0f},
            {"startZ", 120.0f}
        }},
        {"devserver", {
            {"port", 9999}
        }},
        {"shadow", {
            {"mapSize", 4096},
            {"cascades", 3},
            {"biasMin", 0.003f},
            {"biasMax", 0.01f},
            {"intensity", 1.0f},
            {"normalOffset", 0.05f},
            {"pcfRadius", 1.0f},
            {"edgeFade", 0.1f}
        }},
        {"postprocess", {
            {"bloomThreshold", 2.0f},
            {"bloomIntensity", 0.15f},
            {"exposure", 0.85f},
            {"gamma", 2.2f}
        }},
        {"rendering", {
            {"rtShadows", true},
            {"restir", true},
            {"sharc", false},
            {"dlssMode", "quality"}
        }},
        {"pipelineCache", {
            // VkPipelineCache persistence. Relative path is
            // resolved against the current working directory.
            {"enabled", true},
            {"path",    "pipeline_cache.bin"}
        }},
        {"world", {
            {"boundsMin", {-10000.0f, -10000.0f, -10000.0f}},
            {"boundsMax", { 10000.0f,  10000.0f,  10000.0f}}
        }}
    };
}

// ── glm::vec3 helpers ──────────────────────────────────────────────

glm::vec3 Config::getVec3(const std::string& dotPath,
                          const glm::vec3& defaultVal) const
{
    std::string key;
    const nlohmann::json* parent = resolve(dotPath, key);
    if (!parent || !parent->contains(key)) return defaultVal;
    const nlohmann::json& leaf = (*parent)[key];
    if (!leaf.is_array() || leaf.size() < 3) return defaultVal;
    try {
        return { leaf[0].get<float>(),
                 leaf[1].get<float>(),
                 leaf[2].get<float>() };
    } catch (...) {
        return defaultVal;
    }
}

void Config::setVec3(const std::string& dotPath, const glm::vec3& v)
{
    std::string key;
    nlohmann::json* parent = resolveMut(dotPath, key);
    if (parent) {
        (*parent)[key] = nlohmann::json::array({ v.x, v.y, v.z });
    }
}

// ── World bounds ───────────────────────────────────────────────────

WorldBounds Config::worldBounds() const
{
    WorldBounds b;
    b.min = getVec3("world.boundsMin", b.min);
    b.max = getVec3("world.boundsMax", b.max);
    return b;
}

void Config::setWorldBounds(const WorldBounds& b)
{
    setVec3("world.boundsMin", b.min);
    setVec3("world.boundsMax", b.max);
}

bool Config::checkReload()
{
    if (m_path.empty()) return false;

    int64_t ts = getFileTimestamp(m_path);
    if (ts == m_lastModified) return false;

    SV_LOG_INFO("Config", "File changed on disk, reloading: %s", m_path.c_str());

    std::ifstream file(m_path);
    if (!file.is_open()) return false;

    try {
        m_data = nlohmann::json::parse(file);
    } catch (const std::exception& e) {
        SV_LOG_ERROR("Config", "Reload parse error: %s", e.what());
        return false;
    }

    m_lastModified = ts;
    return true;
}

void Config::enableAutoReload(AssetWatcher& watcher, std::function<void(const Config&)> onReload)
{
    if (m_path.empty()) {
        SV_LOG_WARN("Config", "enableAutoReload called but no file loaded");
        return;
    }

    watcher.watch(m_path, [this, onReload]() {
        if (checkReload() && onReload) {
            onReload(*this);
        }
    });

    SV_LOG_INFO("Config", "Auto-reload enabled for: %s", m_path.c_str());
}

const nlohmann::json* Config::resolve(const std::string& dotPath, std::string& leafKey) const
{
    const nlohmann::json* node = &m_data;

    size_t start = 0;
    size_t dot = dotPath.find('.');

    while (dot != std::string::npos) {
        std::string segment = dotPath.substr(start, dot - start);
        if (!node->contains(segment) || !(*node)[segment].is_object()) return nullptr;
        node = &(*node)[segment];
        start = dot + 1;
        dot = dotPath.find('.', start);
    }

    leafKey = dotPath.substr(start);
    return node;
}

nlohmann::json* Config::resolveMut(const std::string& dotPath, std::string& leafKey)
{
    nlohmann::json* node = &m_data;

    size_t start = 0;
    size_t dot = dotPath.find('.');

    while (dot != std::string::npos) {
        std::string segment = dotPath.substr(start, dot - start);
        // Create intermediate objects if they don't exist
        if (!node->contains(segment)) {
            (*node)[segment] = nlohmann::json::object();
        }
        node = &(*node)[segment];
        start = dot + 1;
        dot = dotPath.find('.', start);
    }

    leafKey = dotPath.substr(start);
    return node;
}

int64_t Config::getFileTimestamp(const std::string& path)
{
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return ftime.time_since_epoch().count();
}

} // namespace sv
