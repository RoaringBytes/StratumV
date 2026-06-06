// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── AssetManifest ─────────────────────────────────────────
// JSON-driven asset preload list. The engine parses the manifest at
// startup and exposes it to game code + DLL plugins via
// BaseSystemContext::assetManifest. Plugins look up logical asset
// names and get back a path + kind + freeform metadata. Actual
// VkMesh/VkTexture upload stays in game init code — this module only
// parses and exposes the list.
//
// Schema (v1):
//   {
//     "version": 1,
//     "name": "game_core",
//     "assets": [
//       { "name": "player_mesh",
//         "path": "characters/player.glb",
//         "kind": "mesh",
//         "preload": true },
//       { "name": "ui_font",
//         "path": "fonts/Inter.ttf",
//         "kind": "other",
//         "preload": true,
//         "tags": { "weight": "regular" } }
//     ]
//   }
//
// Layer 4 — depends on: nlohmann::json, EngineLog.
// No Vulkan, no ECS, no VkCtx. Safe for unit tests.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sv {

// ── Asset category ─────────────────────────────────────────────────
// Plugins and the engine use this to route entries to the right
// loader. "Other" is the escape hatch — manifest consumers can still
// look entries up by name and kind string.
enum class AssetKind : uint8_t {
    Mesh     = 0,
    Texture  = 1,
    Audio    = 2,
    Shader   = 3,
    Scene    = 4,
    Material = 5,
    Animation = 6,
    Other    = 7,
};

// Convert between enum and JSON string form. Unknown strings map
// to AssetKind::Other.
AssetKind   parseAssetKind(const std::string& s);
const char* assetKindToString(AssetKind k);

// ── Manifest entry ─────────────────────────────────────────────────
struct AssetEntry {
    std::string    name;              // logical lookup key (unique)
    std::string    path;              // path relative to the manifest file
    AssetKind      kind = AssetKind::Other;
    std::string    kindRaw;           // raw string form (for diagnostics)
    bool           preload = true;    // true = load at startup, false = lazy
    nlohmann::json tags;               // optional freeform metadata (object)
};

// ── AssetManifest ──────────────────────────────────────────────────
class AssetManifest {
public:
    // Load from a JSON file. Returns false on missing file,
    // parse error, or version mismatch.
    bool loadFromFile(const std::string& path);

    // Load directly from an already-parsed JSON document.
    // Useful for tests or in-memory manifests. Returns false on
    // schema errors.
    bool loadFromJson(const nlohmann::json& doc);

    // Drop all entries. Idempotent.
    void clear();

    // ── Lookup ──
    // Find entry by logical name. Returns nullptr if missing.
    const AssetEntry* find(const std::string& name) const;

    // True iff an entry with the given name exists.
    bool contains(const std::string& name) const;

    // All entries, insertion order.
    const std::vector<AssetEntry>& entries() const { return m_entries; }

    // Entries filtered by kind (stable pointer into m_entries).
    std::vector<const AssetEntry*> entriesOfKind(AssetKind k) const;

    // Entries with preload == true.
    std::vector<const AssetEntry*> preloadEntries() const;

    // ── Metadata ──
    const std::string& name()     const { return m_name; }
    const std::string& filePath() const { return m_filePath; }
    int                version()  const { return m_version; }
    size_t             size()     const { return m_entries.size(); }
    bool               empty()    const { return m_entries.empty(); }

private:
    std::vector<AssetEntry>                    m_entries;
    std::unordered_map<std::string, size_t>    m_nameIndex;
    std::string                                m_filePath;
    std::string                                m_name;
    int                                        m_version = 0;
};

} // namespace sv
