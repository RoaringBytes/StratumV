// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "SceneStateVersioning.h"

#include <fstream>
#include <algorithm>
#include <cstdio>

namespace sv {
namespace SceneStateVersioning {

// ── Built-in PostProcess ranges ─────────────────────────────────────

static const std::vector<FieldRange> s_engineRanges = {
    {"postprocess.bloomThreshold", 0.0f, 5.0f, 1.0f},
    {"postprocess.bloomIntensity", 0.0f, 1.0f, 0.5f},
    {"postprocess.exposure",       0.1f, 5.0f, 1.0f},
    {"postprocess.gamma",          1.0f, 3.0f, 2.2f},
};

// ── Migration registry ──────────────────────────────────────────────

static std::vector<std::pair<int, MigrationFn>>& migrations()
{
    static std::vector<std::pair<int, MigrationFn>> s_migrations;
    return s_migrations;
}

void registerMigration(int fromVersion, MigrationFn fn)
{
    migrations().push_back({fromVersion, std::move(fn)});
    std::sort(migrations().begin(), migrations().end(),
              [](auto& a, auto& b) { return a.first < b.first; });
}

void clearMigrations()
{
    migrations().clear();
}

// ── Range registry ──────────────────────────────────────────────────

static std::vector<FieldRange>& gameRanges()
{
    static std::vector<FieldRange> s_gameRanges;
    return s_gameRanges;
}

void registerRanges(const std::vector<FieldRange>& ranges)
{
    gameRanges().insert(gameRanges().end(), ranges.begin(), ranges.end());
}

void clearRanges()
{
    gameRanges().clear();
}

// ── Internal helpers ────────────────────────────────────────────────

// Navigate a dot-separated JSON path. Returns nullptr if path not found.
static nlohmann::json* navigatePath(nlohmann::json& root, const std::string& path)
{
    nlohmann::json* node = &root;
    size_t start = 0;
    while (start < path.size()) {
        auto dot = path.find('.', start);
        auto key = path.substr(start, (dot == std::string::npos) ? std::string::npos : dot - start);
        if (!node->is_object() || !node->contains(key)) return nullptr;
        node = &(*node)[key];
        start = (dot == std::string::npos) ? path.size() : dot + 1;
    }
    return node;
}

// Navigate path, creating missing intermediate objects.  Returns the leaf node.
static nlohmann::json& navigateOrCreate(nlohmann::json& root, const std::string& path)
{
    nlohmann::json* node = &root;
    size_t start = 0;
    while (start < path.size()) {
        auto dot = path.find('.', start);
        auto key = path.substr(start, (dot == std::string::npos) ? std::string::npos : dot - start);
        if (!node->is_object()) *node = nlohmann::json::object();
        node = &(*node)[key];
        start = (dot == std::string::npos) ? path.size() : dot + 1;
    }
    return *node;
}

static void clampField(nlohmann::json& root, const FieldRange& range)
{
    auto* node = navigatePath(root, range.jsonPath);
    if (!node) {
        // Insert default if the parent section exists
        auto lastDot = range.jsonPath.rfind('.');
        if (lastDot != std::string::npos) {
            auto parentPath = range.jsonPath.substr(0, lastDot);
            auto* parent = navigatePath(root, parentPath);
            if (!parent) return;  // parent section missing — skip
        }
        navigateOrCreate(root, range.jsonPath) = range.defaultVal;
        return;
    }
    if (node->is_number()) {
        float val = node->get<float>();
        if (val < range.min || val > range.max) {
            float clamped = std::clamp(val, range.min, range.max);
            fprintf(stderr, "[SceneVersion] Clamped %s: %.3f -> %.3f [%.3f, %.3f]\n",
                    range.jsonPath.c_str(), val, clamped, range.min, range.max);
            *node = clamped;
        }
    }
}

// ── Public API ──────────────────────────────────────────────────────

void validateRanges(nlohmann::json& root)
{
    for (auto& r : s_engineRanges) clampField(root, r);
    for (auto& r : gameRanges())   clampField(root, r);
}

int migrateAndValidate(nlohmann::json& root, const std::string& filePath)
{
    int version = root.value("version", 1);

    for (auto& [fromVer, fn] : migrations()) {
        if (version == fromVer) {
            // Backup before migration
            if (!filePath.empty()) {
                auto backupPath = filePath + ".v" + std::to_string(version) + ".bak";
                try {
                    std::ofstream f(backupPath);
                    if (f.is_open()) f << root.dump(2) << "\n";
                } catch (...) {}
            }

            fn(root);
            version++;
            root["version"] = version;
            printf("[SceneVersion] Migrated v%d -> v%d\n", fromVer, version);
        }
    }

    validateRanges(root);
    return version;
}

} // namespace SceneStateVersioning
} // namespace sv
