// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "SceneStatePersistence.h"

#include <fstream>
#include <filesystem>
#include <cstdio>
#include <vector>

namespace sv {
namespace SceneStatePersistence {

// ── Subsystem registry ──────────────────────────────────────────────

struct SubsystemEntry {
    std::string     key;
    SerializeFn     serialize;
    DeserializeFn   deserialize;
};

static std::vector<SubsystemEntry>& registry()
{
    static std::vector<SubsystemEntry> s_subsystems;
    return s_subsystems;
}

void registerSubsystem(const std::string& key,
                       SerializeFn serialize,
                       DeserializeFn deserialize)
{
    registry().push_back({key, std::move(serialize), std::move(deserialize)});
}

void clearSubsystems()
{
    registry().clear();
}

// ── PostProcess (engine-owned) ──────────────────────────────────────

nlohmann::json serializePostProcess(const PostProcessUBO& pp)
{
    return {
        {"bloomThreshold", pp.bloomThreshold},
        {"bloomIntensity", pp.bloomIntensity},
        {"exposure",       pp.exposure},
        {"gamma",          pp.gamma},
    };
}

void deserializePostProcess(const nlohmann::json& j, PostProcessUBO& pp)
{
    pp.bloomThreshold = j.value("bloomThreshold", pp.bloomThreshold);
    pp.bloomIntensity = j.value("bloomIntensity", pp.bloomIntensity);
    pp.exposure       = j.value("exposure",       pp.exposure);
    pp.gamma          = j.value("gamma",          pp.gamma);
}

// ── Composite ───────────────────────────────────────────────────────

nlohmann::json serializeAll(const PostProcessUBO& pp, int version)
{
    nlohmann::json root;
    root["version"]     = version;
    root["postprocess"] = serializePostProcess(pp);

    for (auto& sub : registry())
        root[sub.key] = sub.serialize();

    return root;
}

void deserializeAll(const nlohmann::json& root, PostProcessUBO& pp)
{
    if (root.contains("postprocess"))
        deserializePostProcess(root["postprocess"], pp);

    for (auto& sub : registry()) {
        if (root.contains(sub.key))
            sub.deserialize(root[sub.key]);
    }
}

// ── File I/O ────────────────────────────────────────────────────────

bool saveToFile(const std::string& path, const nlohmann::json& state)
{
    try {
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent);

        std::ofstream f(path);
        if (!f.is_open()) {
            fprintf(stderr, "[SceneState] Failed to open for writing: %s\n", path.c_str());
            return false;
        }
        f << state.dump(2) << "\n";
        printf("[SceneState] Saved to %s\n", path.c_str());
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[SceneState] Save error: %s\n", e.what());
        return false;
    }
}

nlohmann::json loadFromFile(const std::string& path)
{
    try {
        if (!std::filesystem::exists(path)) return nullptr;

        std::ifstream f(path);
        if (!f.is_open()) return nullptr;

        auto j = nlohmann::json::parse(f);
        printf("[SceneState] Loaded from %s\n", path.c_str());
        return j;
    } catch (const std::exception& e) {
        fprintf(stderr, "[SceneState] Load error: %s\n", e.what());
        return nullptr;
    }
}

} // namespace SceneStatePersistence
} // namespace sv
