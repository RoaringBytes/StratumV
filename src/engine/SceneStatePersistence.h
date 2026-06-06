// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// Engine-generic scene state persistence.
// Provides: PostProcess serialization (engine-owned), file I/O,
// and subsystem registration for game-specific state.
//
// Games register their own subsystems (scene, terrain, ocean, player, etc.)
// via registerSubsystem(). serializeAll/deserializeAll combine engine-owned
// and registered game subsystems into a versioned JSON document.

#include "PostProcess.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace sv {
namespace SceneStatePersistence {

    // ── PostProcess (engine-owned) ──────────────────────────────────
    nlohmann::json serializePostProcess(const PostProcessUBO& pp);
    void deserializePostProcess(const nlohmann::json& j, PostProcessUBO& pp);

    // ── File I/O ────────────────────────────────────────────────────
    // Write JSON to file (creates parent dirs, 2-space indent).
    bool saveToFile(const std::string& path, const nlohmann::json& state);

    // Read JSON from file.  Returns null json if file missing or parse error.
    nlohmann::json loadFromFile(const std::string& path);

    // ── Subsystem Registration (game extension) ─────────────────────
    using SerializeFn   = std::function<nlohmann::json()>;
    using DeserializeFn = std::function<void(const nlohmann::json&)>;

    // Register a named subsystem. Key becomes a top-level JSON field.
    // Game calls this at init time (e.g. "scene", "terrain", "ocean").
    void registerSubsystem(const std::string& key,
                           SerializeFn serialize,
                           DeserializeFn deserialize);

    // Remove all registered subsystems (call at shutdown).
    void clearSubsystems();

    // ── Composite Serialize / Deserialize ───────────────────────────
    // Combines engine subsystems (postprocess) + all registered game subsystems.
    // version: schema version written into the root JSON.
    nlohmann::json serializeAll(const PostProcessUBO& pp, int version);

    // Deserialize engine subsystems + all registered game subsystems.
    void deserializeAll(const nlohmann::json& root, PostProcessUBO& pp);

} // namespace SceneStatePersistence
} // namespace sv
