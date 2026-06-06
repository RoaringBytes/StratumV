// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// Engine-generic scene state versioning and migration framework.
// Provides: schema version tracking, migration registration,
// range validation for engine-owned fields, and registration
// for game-specific field ranges.
//
// Games register their own migrations and field ranges at init time.

#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

namespace sv {
namespace SceneStateVersioning {

    // Engine-only schema version (games set the combined version via migrations).
    constexpr int ENGINE_SCHEMA_VERSION = 1;

    // ── Migration Registration ──────────────────────────────────────
    // Callback receives the root JSON document for in-place modification.
    using MigrationFn = std::function<void(nlohmann::json&)>;

    // Register a migration from version N to N+1.
    // Migrations are applied in order during migrateAndValidate().
    void registerMigration(int fromVersion, MigrationFn fn);
    void clearMigrations();

    // ── Range Validation ────────────────────────────────────────────
    struct FieldRange {
        std::string jsonPath;   // dot-separated (e.g. "postprocess.exposure")
        float       min;
        float       max;
        float       defaultVal;
    };

    // Register additional field ranges (game extension).
    // Engine-owned PostProcess ranges are built-in.
    void registerRanges(const std::vector<FieldRange>& ranges);
    void clearRanges();

    // ── Apply ───────────────────────────────────────────────────────
    // Apply all needed migrations (v1->v2->...) and validate ranges.
    // Creates a backup before each migration: <filePath>.v<N>.bak
    // Returns the final version number.
    int migrateAndValidate(nlohmann::json& root, const std::string& filePath);

    // Clamp all registered fields to their declared ranges.
    // Missing fields get default values. Out-of-range values are clamped.
    void validateRanges(nlohmann::json& root);

} // namespace SceneStateVersioning
} // namespace sv
