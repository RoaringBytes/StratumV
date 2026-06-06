// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── AssetManifest unit tests ───────────────────────────────
// Tests for sv::AssetManifest (src/engine/AssetManifest.h/.cpp).
//
// Covers:
//   - JSON parse happy path (loadFromFile + loadFromJson)
//   - Entry lookup by name (find + contains)
//   - Kind filtering
//   - Preload filtering
//   - Version gate
//   - Malformed / missing file
//   - Tags metadata pass-through
//   - Unknown "kind" string → AssetKind::Other

#include "AssetManifest.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using sv::AssetKind;
using sv::AssetEntry;
using sv::AssetManifest;
using sv::parseAssetKind;
using sv::assetKindToString;
using Catch::Matchers::WithinAbs;

namespace {

std::string fixturePath(const char* name) {
    return std::string(SV_TEST_FIXTURES_DIR) + "/" + name;
}

} // anonymous

// ── Enum <-> string round-trip ──────────────────────────────────────

TEST_CASE("AssetManifest: parseAssetKind known strings",
          "[asset-manifest]") {
    REQUIRE(parseAssetKind("mesh")      == AssetKind::Mesh);
    REQUIRE(parseAssetKind("texture")   == AssetKind::Texture);
    REQUIRE(parseAssetKind("audio")     == AssetKind::Audio);
    REQUIRE(parseAssetKind("shader")    == AssetKind::Shader);
    REQUIRE(parseAssetKind("scene")     == AssetKind::Scene);
    REQUIRE(parseAssetKind("material")  == AssetKind::Material);
    REQUIRE(parseAssetKind("animation") == AssetKind::Animation);
    REQUIRE(parseAssetKind("other")     == AssetKind::Other);
}

TEST_CASE("AssetManifest: parseAssetKind unknown string maps to Other",
          "[asset-manifest]") {
    REQUIRE(parseAssetKind("")             == AssetKind::Other);
    REQUIRE(parseAssetKind("xenoform")     == AssetKind::Other);
    REQUIRE(parseAssetKind("MESH") /*case*/ == AssetKind::Other);
}

TEST_CASE("AssetManifest: assetKindToString matches parseAssetKind",
          "[asset-manifest]") {
    // Round-trip each known enum value through both directions
    AssetKind kinds[] = {
        AssetKind::Mesh,      AssetKind::Texture, AssetKind::Audio,
        AssetKind::Shader,    AssetKind::Scene,   AssetKind::Material,
        AssetKind::Animation, AssetKind::Other
    };
    for (AssetKind k : kinds) {
        const char* s = assetKindToString(k);
        REQUIRE(parseAssetKind(s) == k);
    }
}

// ── Default state ───────────────────────────────────────────────────

TEST_CASE("AssetManifest: default-constructed manifest is empty",
          "[asset-manifest]") {
    AssetManifest m;
    REQUIRE(m.empty());
    REQUIRE(m.size() == 0);
    REQUIRE(m.version() == 0);
    REQUIRE(m.name().empty());
    REQUIRE(m.filePath().empty());
    REQUIRE(m.find("anything") == nullptr);
    REQUIRE_FALSE(m.contains("anything"));
    REQUIRE(m.entriesOfKind(AssetKind::Mesh).empty());
    REQUIRE(m.preloadEntries().empty());
}

// ── loadFromFile: happy path ────────────────────────────────────────

TEST_CASE("AssetManifest: loads manifest_sample fixture",
          "[asset-manifest]") {
    AssetManifest m;
    REQUIRE(m.loadFromFile(fixturePath("manifest_sample.json")));

    REQUIRE(m.version() == 1);
    REQUIRE(m.name() == "test_core");
    REQUIRE(m.size() == 6);
    REQUIRE_FALSE(m.empty());
    REQUIRE(m.filePath() == fixturePath("manifest_sample.json"));
}

TEST_CASE("AssetManifest: find by name returns populated entry",
          "[asset-manifest]") {
    AssetManifest m;
    REQUIRE(m.loadFromFile(fixturePath("manifest_sample.json")));

    const AssetEntry* player = m.find("player_mesh");
    REQUIRE(player != nullptr);
    REQUIRE(player->name == "player_mesh");
    REQUIRE(player->path == "characters/player.glb");
    REQUIRE(player->kind == AssetKind::Mesh);
    REQUIRE(player->kindRaw == "mesh");
    REQUIRE(player->preload == true);

    REQUIRE(m.contains("player_mesh"));
    REQUIRE_FALSE(m.contains("missing_asset"));
    REQUIRE(m.find("missing_asset") == nullptr);
}

TEST_CASE("AssetManifest: entriesOfKind groups by AssetKind",
          "[asset-manifest]") {
    AssetManifest m;
    REQUIRE(m.loadFromFile(fixturePath("manifest_sample.json")));

    auto meshes = m.entriesOfKind(AssetKind::Mesh);
    REQUIRE(meshes.size() == 2);
    // Order matches insertion order in the fixture.
    REQUIRE(meshes[0]->name == "player_mesh");
    REQUIRE(meshes[1]->name == "enemy_mesh");

    auto textures = m.entriesOfKind(AssetKind::Texture);
    REQUIRE(textures.size() == 1);
    REQUIRE(textures[0]->name == "grass_albedo");

    auto audio = m.entriesOfKind(AssetKind::Audio);
    REQUIRE(audio.size() == 1);
    REQUIRE(audio[0]->name == "wind_loop");

    auto scenes = m.entriesOfKind(AssetKind::Scene);
    REQUIRE(scenes.size() == 1);
    REQUIRE(scenes[0]->name == "level_01");

    // ui_font has no "kind" field → defaults to Other
    auto others = m.entriesOfKind(AssetKind::Other);
    REQUIRE(others.size() == 1);
    REQUIRE(others[0]->name == "ui_font");

    // Shaders, materials, animations not in the fixture
    REQUIRE(m.entriesOfKind(AssetKind::Shader).empty());
    REQUIRE(m.entriesOfKind(AssetKind::Material).empty());
    REQUIRE(m.entriesOfKind(AssetKind::Animation).empty());
}

TEST_CASE("AssetManifest: preloadEntries filters preload flag",
          "[asset-manifest]") {
    AssetManifest m;
    REQUIRE(m.loadFromFile(fixturePath("manifest_sample.json")));

    auto preload = m.preloadEntries();
    // Fixture: 4 with preload=true, 2 with preload=false
    REQUIRE(preload.size() == 4);

    std::vector<std::string> names;
    for (auto* e : preload) names.push_back(e->name);

    // ui_font has no explicit preload field — it should default to true
    bool hasUiFont = false;
    for (const auto& n : names) if (n == "ui_font") hasUiFont = true;
    REQUIRE(hasUiFont);
}

TEST_CASE("AssetManifest: tags metadata preserved for wind_loop",
          "[asset-manifest]") {
    AssetManifest m;
    REQUIRE(m.loadFromFile(fixturePath("manifest_sample.json")));

    const AssetEntry* wind = m.find("wind_loop");
    REQUIRE(wind != nullptr);
    REQUIRE(wind->tags.is_object());
    REQUIRE(wind->tags.contains("bus"));
    REQUIRE(wind->tags["bus"].get<std::string>() == "ambience");
    REQUIRE(wind->tags.contains("volume"));
    REQUIRE_THAT(wind->tags["volume"].get<float>(),
                 WithinAbs(0.6f, 1e-5f));
}

TEST_CASE("AssetManifest: missing 'kind' field defaults to Other",
          "[asset-manifest]") {
    AssetManifest m;
    REQUIRE(m.loadFromFile(fixturePath("manifest_sample.json")));

    const AssetEntry* font = m.find("ui_font");
    REQUIRE(font != nullptr);
    REQUIRE(font->kind == AssetKind::Other);
    REQUIRE(font->kindRaw.empty());
    REQUIRE(font->preload == true);
}

// ── Empty / missing / malformed ─────────────────────────────────────

TEST_CASE("AssetManifest: loads manifest_empty fixture",
          "[asset-manifest]") {
    AssetManifest m;
    REQUIRE(m.loadFromFile(fixturePath("manifest_empty.json")));
    REQUIRE(m.version() == 1);
    REQUIRE(m.name() == "empty_manifest");
    REQUIRE(m.size() == 0);
    REQUIRE(m.empty());
    REQUIRE(m.preloadEntries().empty());
    REQUIRE(m.entriesOfKind(AssetKind::Mesh).empty());
}

TEST_CASE("AssetManifest: missing file returns false and stays empty",
          "[asset-manifest]") {
    AssetManifest m;
    REQUIRE_FALSE(m.loadFromFile(fixturePath("definitely_missing.json")));
    REQUIRE(m.empty());
    REQUIRE(m.version() == 0);
}

TEST_CASE("AssetManifest: unsupported version is rejected",
          "[asset-manifest]") {
    AssetManifest m;
    REQUIRE_FALSE(m.loadFromFile(fixturePath("manifest_bad_version.json")));
    REQUIRE(m.empty());
    // On failure we should be back at a clean state (version was
    // reset by loadFromJson before the version check failed).
    REQUIRE(m.version() == 0);
}

// ── loadFromJson direct ─────────────────────────────────────────────

TEST_CASE("AssetManifest: loadFromJson accepts in-memory doc",
          "[asset-manifest]") {
    nlohmann::json doc = {
        {"version", 1},
        {"name", "inline_test"},
        {"assets", {
            {{"name", "a"}, {"path", "a.bin"}, {"kind", "mesh"}},
            {{"name", "b"}, {"path", "b.png"}, {"kind", "texture"},
                           {"preload", false}}
        }}
    };

    AssetManifest m;
    REQUIRE(m.loadFromJson(doc));
    REQUIRE(m.size() == 2);
    REQUIRE(m.find("a")->kind == AssetKind::Mesh);
    REQUIRE(m.find("b")->preload == false);
}

TEST_CASE("AssetManifest: loadFromJson rejects missing version field",
          "[asset-manifest]") {
    nlohmann::json doc = {
        {"name", "no_version"},
        {"assets", nlohmann::json::array()}
    };

    AssetManifest m;
    REQUIRE_FALSE(m.loadFromJson(doc));
    REQUIRE(m.empty());
}

TEST_CASE("AssetManifest: loadFromJson drops entries missing 'name' or 'path'",
          "[asset-manifest]") {
    nlohmann::json doc = {
        {"version", 1},
        {"assets", {
            {{"name", "valid"}, {"path", "ok.bin"}},
            {{"name", "no_path"}},                     // dropped
            {{"path", "no_name.bin"}},                 // dropped
            nlohmann::json::array(),                    // non-object, dropped
            {{"name", "valid2"}, {"path", "ok2.bin"}}
        }}
    };

    AssetManifest m;
    REQUIRE(m.loadFromJson(doc));
    REQUIRE(m.size() == 2);
    REQUIRE(m.contains("valid"));
    REQUIRE(m.contains("valid2"));
    REQUIRE_FALSE(m.contains("no_path"));
    REQUIRE_FALSE(m.contains("no_name"));
}

TEST_CASE("AssetManifest: duplicate names keep the first entry",
          "[asset-manifest]") {
    nlohmann::json doc = {
        {"version", 1},
        {"assets", {
            {{"name", "shared"}, {"path", "first.bin"}},
            {{"name", "shared"}, {"path", "second.bin"}}
        }}
    };

    AssetManifest m;
    REQUIRE(m.loadFromJson(doc));
    REQUIRE(m.size() == 1);
    REQUIRE(m.find("shared")->path == "first.bin");
}

TEST_CASE("AssetManifest: clear resets state fully",
          "[asset-manifest]") {
    AssetManifest m;
    REQUIRE(m.loadFromFile(fixturePath("manifest_sample.json")));
    REQUIRE_FALSE(m.empty());

    m.clear();
    REQUIRE(m.empty());
    REQUIRE(m.size() == 0);
    REQUIRE(m.version() == 0);
    REQUIRE(m.name().empty());
    REQUIRE(m.filePath().empty());
    REQUIRE(m.find("player_mesh") == nullptr);
}
