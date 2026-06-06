// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── AssetBrowser unit tests ─────────────────────────────
// Pure-logic tests for sv::AssetBrowser + kindFromFilename +
// ImportSettings. Exercises:
//   - Extension routing (case-insensitive, compound .scene.json)
//   - isIgnoredFilename (dotfiles, .meta.json, tmp)
//   - scan() on a small fixture tree: asset_browser/{meshes,textures,
//     scenes,audio,shaders,nested/props,misc}
//   - Recursive directory walk + .meta.json filtering
//   - Missing-directory fallback
//   - entriesOfKind + filter() substring + findByRelativePath
//   - ImportSettings JSON round-trip + in-memory cache
//   - populateManifest → AssetManifest bridge
//
// Fixtures live under tests/fixtures/asset_browser/. The contents of
// every "binary" stub are small text blobs — the browser only reads
// filename + size + mtime, so payload bytes are irrelevant.

#include "AssetBrowser.h"
#include "AssetManifest.h"
#include "test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using sv::AssetBrowser;
using sv::AssetBrowserEntry;
using sv::AssetKind;
using sv::AssetManifest;
using sv::ImportSettings;
using sv::kindFromFilename;
using sv::isIgnoredFilename;
using svtest::TempDir;
using svtest::writeFile;
using Catch::Matchers::WithinAbs;

namespace {

std::string browserRoot()
{
    return std::string(SV_TEST_FIXTURES_DIR) + "/asset_browser";
}

// Helper: find entry by relativePath in the vector-of-pointers form.
const AssetBrowserEntry* findByRel(
    const std::vector<const AssetBrowserEntry*>& v,
    const std::string& rel)
{
    for (const auto* e : v) if (e->relativePath == rel) return e;
    return nullptr;
}

} // anonymous

// ── kindFromFilename: extension routing ────────────────────────────

TEST_CASE("kindFromFilename: mesh extensions route to Mesh",
          "[asset-browser][kind]") {
    REQUIRE(kindFromFilename("player.glb")   == AssetKind::Mesh);
    REQUIRE(kindFromFilename("enemy.fbx")    == AssetKind::Mesh);
    REQUIRE(kindFromFilename("tree.gltf")    == AssetKind::Mesh);
    REQUIRE(kindFromFilename("prop.obj")     == AssetKind::Mesh);
}

TEST_CASE("kindFromFilename: texture extensions route to Texture",
          "[asset-browser][kind]") {
    REQUIRE(kindFromFilename("albedo.png")     == AssetKind::Texture);
    REQUIRE(kindFromFilename("normal.jpg")     == AssetKind::Texture);
    REQUIRE(kindFromFilename("mask.tga")       == AssetKind::Texture);
    REQUIRE(kindFromFilename("compressed.ktx2")== AssetKind::Texture);
    REQUIRE(kindFromFilename("cube.dds")       == AssetKind::Texture);
    REQUIRE(kindFromFilename("sky.hdr")        == AssetKind::Texture);
}

TEST_CASE("kindFromFilename: audio/shader/material/animation routing",
          "[asset-browser][kind]") {
    REQUIRE(kindFromFilename("wind.ogg")     == AssetKind::Audio);
    REQUIRE(kindFromFilename("voice.wav")    == AssetKind::Audio);
    REQUIRE(kindFromFilename("vs.vert")      == AssetKind::Shader);
    REQUIRE(kindFromFilename("fs.frag")      == AssetKind::Shader);
    REQUIRE(kindFromFilename("blit.comp")    == AssetKind::Shader);
    REQUIRE(kindFromFilename("foo.spv")      == AssetKind::Shader);
    REQUIRE(kindFromFilename("pbr.mat")      == AssetKind::Material);
    REQUIRE(kindFromFilename("walk.anim")    == AssetKind::Animation);
}

TEST_CASE("kindFromFilename: compound .scene.json routes to Scene",
          "[asset-browser][kind]") {
    REQUIRE(kindFromFilename("level_01.scene.json") == AssetKind::Scene);
    REQUIRE(kindFromFilename("test.SCENE.json")     == AssetKind::Scene);
    // A bare .json with no .scene prefix is Other
    REQUIRE(kindFromFilename("data.json")           == AssetKind::Other);
}

TEST_CASE("kindFromFilename: compound .thumb.png routes to Other",
          "[asset-browser][kind]") {
    // Baked thumbnails must NOT be routed as Texture — they are
    // metadata and are paired with isIgnoredFilename() to be dropped
    // from the browser entirely.
    REQUIRE(kindFromFilename("player.glb.thumb.png") == AssetKind::Other);
    REQUIRE(kindFromFilename("MESH.GLB.THUMB.PNG")   == AssetKind::Other);
    // A normal .png is still Texture.
    REQUIRE(kindFromFilename("diffuse.png")          == AssetKind::Texture);
}

TEST_CASE("kindFromFilename: case-insensitive extension matching",
          "[asset-browser][kind]") {
    REQUIRE(kindFromFilename("SKY.JPG")     == AssetKind::Texture);
    REQUIRE(kindFromFilename("Player.GLB")  == AssetKind::Mesh);
    REQUIRE(kindFromFilename("Rig.FBX")     == AssetKind::Mesh);
}

TEST_CASE("kindFromFilename: unknown extension falls back to Other",
          "[asset-browser][kind]") {
    REQUIRE(kindFromFilename("README.txt")   == AssetKind::Other);
    REQUIRE(kindFromFilename("blob.xyz")     == AssetKind::Other);
    REQUIRE(kindFromFilename("no_ext")       == AssetKind::Other);
    REQUIRE(kindFromFilename("")             == AssetKind::Other);
}

// ── isIgnoredFilename ──────────────────────────────────────────────

TEST_CASE("isIgnoredFilename: skips meta.json, thumb.png, dotfiles, tmp files",
          "[asset-browser][ignore]") {
    REQUIRE(isIgnoredFilename("player.meta.json"));
    REQUIRE(isIgnoredFilename("config.META.JSON"));
    // baked thumbnails must be filtered too.
    REQUIRE(isIgnoredFilename("player.glb.thumb.png"));
    REQUIRE(isIgnoredFilename("TREE.FBX.THUMB.PNG"));
    REQUIRE(isIgnoredFilename(".DS_Store"));
    REQUIRE(isIgnoredFilename(".gitignore"));
    REQUIRE(isIgnoredFilename("scratch.tmp"));
    REQUIRE(isIgnoredFilename(""));

    REQUIRE_FALSE(isIgnoredFilename("player.glb"));
    REQUIRE_FALSE(isIgnoredFilename("level.scene.json"));
    REQUIRE_FALSE(isIgnoredFilename("README.txt"));
    // Plain .png textures must NOT be ignored.
    REQUIRE_FALSE(isIgnoredFilename("diffuse.png"));
}

// ── AssetBrowser: default state ────────────────────────────────────

TEST_CASE("AssetBrowser: default-constructed browser is empty",
          "[asset-browser]") {
    AssetBrowser b;
    REQUIRE(b.empty());
    REQUIRE(b.size() == 0);
    REQUIRE(b.rootDir().empty());
    REQUIRE(b.entries().empty());
    REQUIRE(b.findByRelativePath("anything") == nullptr);
    REQUIRE(b.entriesOfKind(AssetKind::Mesh).empty());
    REQUIRE_FALSE(b.rescan()); // no prior scan root → false
}

// ── AssetBrowser: scan the fixture tree ────────────────────────────

TEST_CASE("AssetBrowser: scan discovers fixture tree",
          "[asset-browser][scan]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));
    REQUIRE_FALSE(b.empty());
    REQUIRE(b.rootDir().find("asset_browser") != std::string::npos);

    // Fixture has 11 files total; player.meta.json is filtered out →
    // 10 visible entries.
    REQUIRE(b.size() == 10);

    // Every entry has populated name + relativePath + absolutePath,
    // and relativePath uses forward slashes.
    for (const auto& e : b.entries()) {
        REQUIRE_FALSE(e.name.empty());
        REQUIRE_FALSE(e.relativePath.empty());
        REQUIRE_FALSE(e.absolutePath.empty());
        REQUIRE(e.relativePath.find('\\') == std::string::npos);
        REQUIRE(e.absolutePath.find('\\') == std::string::npos);
    }
}

TEST_CASE("AssetBrowser: scan routes fixture files to expected kinds",
          "[asset-browser][scan]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));

    auto meshes = b.entriesOfKind(AssetKind::Mesh);
    // player.glb, enemy.fbx, tree.gltf, nested/props/crate.glb
    REQUIRE(meshes.size() == 4);
    REQUIRE(findByRel(meshes, "meshes/player.glb")      != nullptr);
    REQUIRE(findByRel(meshes, "meshes/enemy.fbx")       != nullptr);
    REQUIRE(findByRel(meshes, "meshes/tree.gltf")       != nullptr);
    REQUIRE(findByRel(meshes, "nested/props/crate.glb") != nullptr);

    auto textures = b.entriesOfKind(AssetKind::Texture);
    REQUIRE(textures.size() == 2);
    REQUIRE(findByRel(textures, "textures/grass_albedo.png") != nullptr);
    // Uppercase SKY.JPG must still be routed as Texture
    REQUIRE(findByRel(textures, "textures/SKY.JPG")          != nullptr);

    auto scenes = b.entriesOfKind(AssetKind::Scene);
    REQUIRE(scenes.size() == 1);
    REQUIRE(scenes[0]->relativePath == "scenes/level_01.scene.json");

    auto audio = b.entriesOfKind(AssetKind::Audio);
    REQUIRE(audio.size() == 1);
    REQUIRE(audio[0]->name == "wind_loop.ogg");

    auto shaders = b.entriesOfKind(AssetKind::Shader);
    REQUIRE(shaders.size() == 1);
    REQUIRE(shaders[0]->relativePath == "shaders/test.vert");

    auto others = b.entriesOfKind(AssetKind::Other);
    REQUIRE(others.size() == 1);
    REQUIRE(others[0]->relativePath == "misc/README.txt");
}

TEST_CASE("AssetBrowser: recursive scan finds nested files",
          "[asset-browser][scan]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));

    const AssetBrowserEntry* nested =
        b.findByRelativePath("nested/props/crate.glb");
    REQUIRE(nested != nullptr);
    REQUIRE(nested->kind == AssetKind::Mesh);
    REQUIRE(nested->extension == ".glb");
    // lastModified should be populated by the scanner for real files.
    REQUIRE(nested->lastModified != 0);
}

TEST_CASE("AssetBrowser: scan filters .meta.json sidecars",
          "[asset-browser][scan]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));

    // player.meta.json exists on disk but must never show up in the
    // browser because isIgnoredFilename() drops it.
    REQUIRE(b.findByRelativePath("meshes/player.meta.json") == nullptr);
    for (const auto& e : b.entries()) {
        REQUIRE(e.name != "player.meta.json");
    }
}

TEST_CASE("AssetBrowser: scan on missing directory returns false and clears",
          "[asset-browser][scan]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));
    REQUIRE_FALSE(b.empty());

    REQUIRE_FALSE(b.scan(browserRoot() + "/does_not_exist_definitely"));
    REQUIRE(b.empty());
    REQUIRE(b.rootDir().empty());
}

// ── AssetBrowser: filter + lookup ──────────────────────────────────

TEST_CASE("AssetBrowser: filter substring is case-insensitive",
          "[asset-browser][filter]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));

    auto grassHits = b.filter("GRASS");
    REQUIRE(grassHits.size() == 1);
    REQUIRE(grassHits[0]->name == "grass_albedo.png");

    // "meshes" matches every entry in meshes/ (3 in that folder)
    auto meshFolder = b.filter("meshes/");
    REQUIRE(meshFolder.size() == 3);

    // Empty needle returns every entry (10 total post-filter of .meta.json)
    auto all = b.filter("");
    REQUIRE(all.size() == b.size());

    // No match yields empty result
    auto none = b.filter("definitely_not_there");
    REQUIRE(none.empty());
}

TEST_CASE("AssetBrowser: findByRelativePath exact match required",
          "[asset-browser][lookup]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));

    REQUIRE(b.findByRelativePath("meshes/player.glb") != nullptr);
    // Case mismatch → no match (forward-slash path is literal)
    REQUIRE(b.findByRelativePath("Meshes/player.glb") == nullptr);
    // Missing file
    REQUIRE(b.findByRelativePath("meshes/missing.glb") == nullptr);
}

TEST_CASE("AssetBrowser: entries sorted by relativePath for determinism",
          "[asset-browser][scan]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));

    const auto& entries = b.entries();
    for (size_t i = 1; i < entries.size(); ++i) {
        REQUIRE(entries[i - 1].relativePath <= entries[i].relativePath);
    }
}

// ── AssetBrowser: rescan preserves import cache ────────────────────

TEST_CASE("AssetBrowser: rescan preserves in-memory import cache",
          "[asset-browser][import]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));

    ImportSettings s;
    s.scale           = 2.5f;
    s.upAxis          = ImportSettings::UpAxis::Z;
    s.materialMapping = "pbr";
    s.preload         = false;
    b.setImportSettings("meshes/player.glb", s);
    REQUIRE(b.hasImportSettings("meshes/player.glb"));

    REQUIRE(b.rescan());
    // Rescan must not blow away user-authored import settings.
    REQUIRE(b.hasImportSettings("meshes/player.glb"));
    auto got = b.getImportSettings("meshes/player.glb");
    REQUIRE_THAT(got.scale, WithinAbs(2.5f, 1e-5f));
    REQUIRE(got.upAxis == ImportSettings::UpAxis::Z);
    REQUIRE(got.materialMapping == "pbr");
    REQUIRE(got.preload == false);

    // clear() wipes both entries and cache.
    b.clear();
    REQUIRE_FALSE(b.hasImportSettings("meshes/player.glb"));
    REQUIRE(b.empty());
}

// ── ImportSettings JSON round-trip ─────────────────────────────────

TEST_CASE("ImportSettings: default-constructed values",
          "[asset-browser][import]") {
    ImportSettings s;
    REQUIRE_THAT(s.scale, WithinAbs(1.0f, 1e-6f));
    REQUIRE(s.upAxis == ImportSettings::UpAxis::Y);
    REQUIRE(s.materialMapping.empty());
    REQUIRE(s.preload == true);
}

TEST_CASE("ImportSettings: toJson -> fromJson round-trip",
          "[asset-browser][import]") {
    ImportSettings s;
    s.scale           = 0.01f;
    s.upAxis          = ImportSettings::UpAxis::Z;
    s.materialMapping = "unlit";
    s.preload         = false;

    nlohmann::json doc = s.toJson();
    REQUIRE(doc["version"].get<int>() == 1);
    REQUIRE(doc["upAxis"].get<std::string>() == "Z");

    ImportSettings back = ImportSettings::fromJson(doc);
    REQUIRE_THAT(back.scale, WithinAbs(0.01f, 1e-6f));
    REQUIRE(back.upAxis == ImportSettings::UpAxis::Z);
    REQUIRE(back.materialMapping == "unlit");
    REQUIRE(back.preload == false);
}

TEST_CASE("ImportSettings: fromJson accepts lowercase upAxis",
          "[asset-browser][import]") {
    nlohmann::json doc = {
        {"version", 1},
        {"scale",   3.0f},
        {"upAxis",  "z"}, // lowercase
        {"preload", true}
    };
    ImportSettings s = ImportSettings::fromJson(doc);
    REQUIRE(s.upAxis == ImportSettings::UpAxis::Z);
    REQUIRE_THAT(s.scale, WithinAbs(3.0f, 1e-6f));
}

TEST_CASE("ImportSettings: fromJson empty doc keeps defaults",
          "[asset-browser][import]") {
    ImportSettings s = ImportSettings::fromJson(nlohmann::json::object());
    REQUIRE_THAT(s.scale, WithinAbs(1.0f, 1e-6f));
    REQUIRE(s.upAxis == ImportSettings::UpAxis::Y);
    REQUIRE(s.preload == true);
}

// ── AssetBrowser: getImportSettings default behavior ──────────────

TEST_CASE("AssetBrowser: getImportSettings returns defaults for unknown path",
          "[asset-browser][import]") {
    AssetBrowser b;
    ImportSettings s = b.getImportSettings("nowhere/missing.glb");
    REQUIRE_THAT(s.scale, WithinAbs(1.0f, 1e-6f));
    REQUIRE(s.upAxis == ImportSettings::UpAxis::Y);
    REQUIRE(s.preload == true);
    REQUIRE_FALSE(b.hasImportSettings("nowhere/missing.glb"));
}

// ── AssetBrowser → AssetManifest bridge ────────────────────────────

TEST_CASE("AssetBrowser: populateManifest produces AssetManifest entries",
          "[asset-browser][manifest]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));

    AssetManifest m;
    b.populateManifest(m, "fixture_browser");

    REQUIRE(m.version() == 1);
    REQUIRE(m.name() == "fixture_browser");
    REQUIRE(m.size() == b.size());

    // Two "player" stems collide: meshes/player.glb wins the bare
    // "player" name; the second (if any) would get parent-dir prefix.
    // Fixture has only one player file, so lookup by stem should work.
    REQUIRE(m.contains("player"));
    const auto* player = m.find("player");
    REQUIRE(player != nullptr);
    REQUIRE(player->path == "meshes/player.glb");
    REQUIRE(player->kind == AssetKind::Mesh);

    // Scene stem should strip both .json and .scene
    REQUIRE(m.contains("level_01"));
    REQUIRE(m.find("level_01")->kind == AssetKind::Scene);

    // Texture with uppercase filename still lands under its stem
    REQUIRE(m.contains("SKY"));
    REQUIRE(m.find("SKY")->kind == AssetKind::Texture);
}

TEST_CASE("AssetBrowser: populateManifest honors import-cache preload flag",
          "[asset-browser][manifest]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));

    ImportSettings s;
    s.preload = false;
    b.setImportSettings("meshes/player.glb", s);

    AssetManifest m;
    b.populateManifest(m);

    // preload=false in the cache must propagate through the JSON bridge.
    const auto* player = m.find("player");
    REQUIRE(player != nullptr);
    REQUIRE(player->preload == false);

    // Other entries without cache entries default to preload=true.
    const auto* tree = m.find("tree");
    REQUIRE(tree != nullptr);
    REQUIRE(tree->preload == true);
}

// ── .meta.json sidecar persistence ──────────────────────
// Pre-seeded fixture: tests/fixtures/asset_browser/meshes/enemy.fbx.meta.json
//   { version:1, scale:0.01, upAxis:"Z", materialMapping:"pbr", preload:false }

TEST_CASE("AssetBrowser: scan auto-loads fixture .meta.json sidecar",
          "[asset-browser][meta]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));
    // enemy.fbx.meta.json sits next to enemy.fbx and should have been
    // auto-loaded into the cache during scan().
    REQUIRE(b.hasImportSettings("meshes/enemy.fbx"));
    auto s = b.getImportSettings("meshes/enemy.fbx");
    REQUIRE_THAT(s.scale, WithinAbs(0.01f, 1e-6f));
    REQUIRE(s.upAxis == ImportSettings::UpAxis::Z);
    REQUIRE(s.materialMapping == "pbr");
    REQUIRE(s.preload == false);
}

TEST_CASE("AssetBrowser: scan auto-load is cache-wins for in-memory edits",
          "[asset-browser][meta]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));

    // Overwrite cache entry with different values and rescan — the on-disk
    // sidecar must NOT clobber the uncommitted in-memory edit.
    ImportSettings tweaked;
    tweaked.scale  = 7.5f;
    tweaked.upAxis = ImportSettings::UpAxis::Y;
    tweaked.preload = true;
    b.setImportSettings("meshes/enemy.fbx", tweaked);
    REQUIRE(b.rescan());

    auto got = b.getImportSettings("meshes/enemy.fbx");
    REQUIRE_THAT(got.scale, WithinAbs(7.5f, 1e-6f));
    REQUIRE(got.upAxis == ImportSettings::UpAxis::Y);
    REQUIRE(got.preload == true);
}

TEST_CASE("AssetBrowser: auto-load can be disabled before scan",
          "[asset-browser][meta]") {
    AssetBrowser b;
    b.setAutoLoadMeta(false);
    REQUIRE_FALSE(b.autoLoadMeta());
    REQUIRE(b.scan(browserRoot()));
    // With auto-load off, the fixture sidecar stays on disk but is
    // never read — the cache remains empty for that entry.
    REQUIRE_FALSE(b.hasImportSettings("meshes/enemy.fbx"));
}

TEST_CASE("AssetBrowser: metaFilePathFor returns '<absPath>.meta.json'",
          "[asset-browser][meta]") {
    AssetBrowser b;
    REQUIRE(b.scan(browserRoot()));

    std::string meta = b.metaFilePathFor("meshes/enemy.fbx");
    REQUIRE_FALSE(meta.empty());
    // Must end with ".meta.json" and include "enemy.fbx" verbatim.
    REQUIRE(meta.size() >= 10);
    REQUIRE(meta.substr(meta.size() - 10) == ".meta.json");
    REQUIRE(meta.find("enemy.fbx.meta.json") != std::string::npos);

    // Unknown relativePath → empty.
    REQUIRE(b.metaFilePathFor("not/real.glb").empty());
}

TEST_CASE("AssetBrowser: saveMetaFile and loadMetaFile round-trip via temp dir",
          "[asset-browser][meta]") {
    TempDir tmp("browser_save_load_roundtrip");

    // Seed the temp dir with a single stub asset.
    writeFile(tmp.path / "sub" / "rig.glb", "ignored-payload");

    AssetBrowser b;
    REQUIRE(b.scan(tmp.str()));
    REQUIRE(b.findByRelativePath("sub/rig.glb") != nullptr);

    // Populate the cache and save to disk.
    ImportSettings s;
    s.scale           = 3.14f;
    s.upAxis          = ImportSettings::UpAxis::Z;
    s.materialMapping = "unlit";
    s.preload         = false;
    b.setImportSettings("sub/rig.glb", s);
    REQUIRE(b.saveMetaFile("sub/rig.glb"));

    // Sidecar file must exist on disk.
    std::filesystem::path sidecar = tmp.path / "sub" / "rig.glb.meta.json";
    REQUIRE(std::filesystem::exists(sidecar));

    // Fresh browser + load from disk must see the same values.
    AssetBrowser b2;
    b2.setAutoLoadMeta(false); // force explicit loadMetaFile call
    REQUIRE(b2.scan(tmp.str()));
    REQUIRE_FALSE(b2.hasImportSettings("sub/rig.glb"));
    REQUIRE(b2.loadMetaFile("sub/rig.glb"));
    REQUIRE(b2.hasImportSettings("sub/rig.glb"));

    auto loaded = b2.getImportSettings("sub/rig.glb");
    REQUIRE_THAT(loaded.scale, WithinAbs(3.14f, 1e-6f));
    REQUIRE(loaded.upAxis == ImportSettings::UpAxis::Z);
    REQUIRE(loaded.materialMapping == "unlit");
    REQUIRE(loaded.preload == false);
}

TEST_CASE("AssetBrowser: saveMetaFile writes defaults when cache is empty",
          "[asset-browser][meta]") {
    TempDir tmp("browser_save_defaults");
    writeFile(tmp.path / "mesh.glb");

    AssetBrowser b;
    REQUIRE(b.scan(tmp.str()));
    REQUIRE_FALSE(b.hasImportSettings("mesh.glb"));

    // saveMetaFile should write a default-constructed ImportSettings
    // scaffold even though no cache entry exists for this path.
    REQUIRE(b.saveMetaFile("mesh.glb"));
    std::filesystem::path sidecar = tmp.path / "mesh.glb.meta.json";
    REQUIRE(std::filesystem::exists(sidecar));

    // Parse it back by hand to confirm the shape.
    std::ifstream in(sidecar);
    nlohmann::json doc;
    in >> doc;
    REQUIRE(doc["version"].get<int>() == 1);
    REQUIRE(doc["upAxis"].get<std::string>() == "Y");
    REQUIRE(doc["preload"].get<bool>() == true);
    REQUIRE_THAT(doc["scale"].get<float>(), WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("AssetBrowser: saveMetaFile / loadMetaFile reject unknown paths",
          "[asset-browser][meta]") {
    TempDir tmp("browser_unknown_path");
    writeFile(tmp.path / "real.glb");

    AssetBrowser b;
    REQUIRE(b.scan(tmp.str()));

    REQUIRE_FALSE(b.saveMetaFile("does/not/exist.glb"));
    REQUIRE_FALSE(b.loadMetaFile("does/not/exist.glb"));
    REQUIRE_FALSE(b.hasImportSettings("does/not/exist.glb"));
}

TEST_CASE("AssetBrowser: loadMetaFile returns false for missing sidecar",
          "[asset-browser][meta]") {
    TempDir tmp("browser_missing_sidecar");
    writeFile(tmp.path / "lonely.glb");

    AssetBrowser b;
    b.setAutoLoadMeta(false);
    REQUIRE(b.scan(tmp.str()));
    // Known asset but no .meta.json next to it → false, cache unchanged.
    REQUIRE_FALSE(b.loadMetaFile("lonely.glb"));
    REQUIRE_FALSE(b.hasImportSettings("lonely.glb"));
}

TEST_CASE("AssetBrowser: loadMetaFile tolerates invalid JSON",
          "[asset-browser][meta]") {
    TempDir tmp("browser_invalid_json");
    writeFile(tmp.path / "broken.glb");
    // Write a deliberately malformed sidecar next to the stub.
    {
        std::ofstream bad(tmp.path / "broken.glb.meta.json");
        bad << "{ this is : not, valid json ]]";
    }

    AssetBrowser b;
    // First verify auto-load doesn't crash the scan.
    REQUIRE(b.scan(tmp.str()));
    REQUIRE_FALSE(b.hasImportSettings("broken.glb"));

    // Direct call returns false without mutating the cache.
    REQUIRE_FALSE(b.loadMetaFile("broken.glb"));
    REQUIRE_FALSE(b.hasImportSettings("broken.glb"));
}

TEST_CASE("AssetBrowser: auto-save ON writes sidecar on setImportSettings",
          "[asset-browser][meta]") {
    TempDir tmp("browser_auto_save_on");
    writeFile(tmp.path / "char.glb");

    AssetBrowser b;
    // Default-off per API contract.
    REQUIRE_FALSE(b.autoSaveMeta());
    b.setAutoSaveMeta(true);
    REQUIRE(b.autoSaveMeta());
    REQUIRE(b.scan(tmp.str()));

    ImportSettings s;
    s.scale   = 2.0f;
    s.upAxis  = ImportSettings::UpAxis::Z;
    s.preload = false;
    b.setImportSettings("char.glb", s);

    // Sidecar must now exist on disk and round-trip through a fresh browser.
    std::filesystem::path sidecar = tmp.path / "char.glb.meta.json";
    REQUIRE(std::filesystem::exists(sidecar));

    AssetBrowser b2;
    REQUIRE(b2.scan(tmp.str())); // default auto-load reads the sidecar
    REQUIRE(b2.hasImportSettings("char.glb"));
    auto back = b2.getImportSettings("char.glb");
    REQUIRE_THAT(back.scale, WithinAbs(2.0f, 1e-6f));
    REQUIRE(back.upAxis == ImportSettings::UpAxis::Z);
    REQUIRE(back.preload == false);
}

TEST_CASE("AssetBrowser: auto-save OFF (default) leaves the disk untouched",
          "[asset-browser][meta]") {
    TempDir tmp("browser_auto_save_off");
    writeFile(tmp.path / "quiet.glb");

    AssetBrowser b;
    REQUIRE_FALSE(b.autoSaveMeta()); // default is OFF
    REQUIRE(b.scan(tmp.str()));

    ImportSettings s;
    s.scale = 9.9f;
    b.setImportSettings("quiet.glb", s);

    // Sidecar must NOT exist — setImportSettings only touched memory.
    REQUIRE_FALSE(std::filesystem::exists(tmp.path / "quiet.glb.meta.json"));
    // Cache was still updated in-memory.
    REQUIRE(b.hasImportSettings("quiet.glb"));
    REQUIRE_THAT(b.getImportSettings("quiet.glb").scale, WithinAbs(9.9f, 1e-6f));
}
