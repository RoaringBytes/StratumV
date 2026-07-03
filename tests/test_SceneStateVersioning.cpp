// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
//
// Unit tests for SceneStateVersioning: migration chains, backup
// creation, and range validation/clamping. The module keeps static
// registries, so every test clears them up-front and on exit via a
// small RAII guard.

#include <catch2/catch_test_macros.hpp>

#include "SceneStateVersioning.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace ssv = sv::SceneStateVersioning;

namespace {

// Reset the module's static registries around each test.
struct RegistryGuard {
    RegistryGuard()  { ssv::clearMigrations(); ssv::clearRanges(); }
    ~RegistryGuard() { ssv::clearMigrations(); ssv::clearRanges(); }
};

} // namespace

// ── Range validation ────────────────────────────────────────────────

TEST_CASE("SceneStateVersioning: in-range engine fields untouched",
          "[versioning][ranges]") {
    RegistryGuard guard;
    nlohmann::json root;
    root["postprocess"]["exposure"] = 1.5f;
    root["postprocess"]["gamma"]    = 2.2f;

    ssv::validateRanges(root);

    CHECK(root["postprocess"]["exposure"].get<float>() == 1.5f);
    CHECK(root["postprocess"]["gamma"].get<float>() == 2.2f);
}

TEST_CASE("SceneStateVersioning: out-of-range fields are clamped",
          "[versioning][ranges]") {
    RegistryGuard guard;
    nlohmann::json root;
    root["postprocess"]["exposure"] = 99.0f;  // max 5.0
    root["postprocess"]["gamma"]    = 0.1f;   // min 1.0

    ssv::validateRanges(root);

    CHECK(root["postprocess"]["exposure"].get<float>() == 5.0f);
    CHECK(root["postprocess"]["gamma"].get<float>() == 1.0f);
}

TEST_CASE("SceneStateVersioning: missing field in existing section gets default",
          "[versioning][ranges]") {
    RegistryGuard guard;
    nlohmann::json root;
    root["postprocess"]["exposure"] = 1.0f;   // section exists, gamma missing

    ssv::validateRanges(root);

    REQUIRE(root["postprocess"].contains("gamma"));
    CHECK(root["postprocess"]["gamma"].get<float>() == 2.2f);
}

TEST_CASE("SceneStateVersioning: missing parent section is skipped",
          "[versioning][ranges]") {
    RegistryGuard guard;
    nlohmann::json root = nlohmann::json::object();  // no postprocess at all

    ssv::validateRanges(root);

    CHECK_FALSE(root.contains("postprocess"));
}

TEST_CASE("SceneStateVersioning: game-registered ranges are enforced",
          "[versioning][ranges]") {
    RegistryGuard guard;
    ssv::registerRanges({{"game.fogDensity", 0.0f, 1.0f, 0.25f}});

    nlohmann::json root;
    root["game"]["fogDensity"] = 4.0f;

    ssv::validateRanges(root);
    CHECK(root["game"]["fogDensity"].get<float>() == 1.0f);

    ssv::clearRanges();
    root["game"]["fogDensity"] = 4.0f;
    ssv::validateRanges(root);
    CHECK(root["game"]["fogDensity"].get<float>() == 4.0f);  // no longer clamped
}

TEST_CASE("SceneStateVersioning: non-numeric field is left alone",
          "[versioning][ranges]") {
    RegistryGuard guard;
    nlohmann::json root;
    root["postprocess"]["exposure"] = "not-a-number";

    ssv::validateRanges(root);

    CHECK(root["postprocess"]["exposure"].is_string());
}

// ── Migration chains ────────────────────────────────────────────────

TEST_CASE("SceneStateVersioning: no migrations returns stored version",
          "[versioning][migrate]") {
    RegistryGuard guard;
    nlohmann::json root;
    root["version"] = 3;
    CHECK(ssv::migrateAndValidate(root, "") == 3);

    nlohmann::json fresh = nlohmann::json::object();  // missing version -> 1
    CHECK(ssv::migrateAndValidate(fresh, "") == 1);
}

TEST_CASE("SceneStateVersioning: chained migrations run in order",
          "[versioning][migrate]") {
    RegistryGuard guard;
    // Register out of order on purpose; module sorts by fromVersion.
    ssv::registerMigration(2, [](nlohmann::json& j) { j["b"] = true; });
    ssv::registerMigration(1, [](nlohmann::json& j) { j["a"] = true; });

    nlohmann::json root;
    root["version"] = 1;

    const int finalVersion = ssv::migrateAndValidate(root, "");

    CHECK(finalVersion == 3);
    CHECK(root["version"].get<int>() == 3);
    CHECK(root.value("a", false));
    CHECK(root.value("b", false));
}

TEST_CASE("SceneStateVersioning: migrations below current version are skipped",
          "[versioning][migrate]") {
    RegistryGuard guard;
    bool ran = false;
    ssv::registerMigration(1, [&](nlohmann::json&) { ran = true; });

    nlohmann::json root;
    root["version"] = 2;

    CHECK(ssv::migrateAndValidate(root, "") == 2);
    CHECK_FALSE(ran);
    CHECK(root["version"].get<int>() == 2);
}

TEST_CASE("SceneStateVersioning: backup written before each migration",
          "[versioning][migrate]") {
    RegistryGuard guard;
    namespace fs = std::filesystem;

    const fs::path dir = fs::temp_directory_path() / "sv_ssv_test";
    fs::create_directories(dir);
    const std::string file = (dir / "scene_state.json").string();
    const std::string bak  = file + ".v1.bak";
    std::remove(bak.c_str());

    ssv::registerMigration(1, [](nlohmann::json& j) { j["migrated"] = true; });

    nlohmann::json root;
    root["version"] = 1;
    root["payload"] = 42;

    CHECK(ssv::migrateAndValidate(root, file) == 2);
    REQUIRE(fs::exists(bak));

    // The backup must hold the PRE-migration document.
    std::ifstream in(bak);
    nlohmann::json backedUp = nlohmann::json::parse(in);
    CHECK(backedUp["version"].get<int>() == 1);
    CHECK(backedUp["payload"].get<int>() == 42);
    CHECK_FALSE(backedUp.contains("migrated"));

    std::remove(bak.c_str());
    std::error_code ec;
    fs::remove(dir, ec);
}

TEST_CASE("SceneStateVersioning: empty filePath writes no backup",
          "[versioning][migrate]") {
    RegistryGuard guard;
    ssv::registerMigration(1, [](nlohmann::json&) {});

    nlohmann::json root;
    root["version"] = 1;

    // Would throw/create stray files if it tried; just verify it runs.
    CHECK(ssv::migrateAndValidate(root, "") == 2);
}
