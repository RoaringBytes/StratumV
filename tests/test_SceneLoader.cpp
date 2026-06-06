// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── SceneLoader marker filter unit tests ──────────────────
// Tests for sv::filterMarkersByType() and SceneLoader::getSpawnPoints().
//
// SceneLoader::loadFromFile() needs a VkCtx so we cannot exercise the
// full JSON → scene pipeline from a unit test. Instead we verify the
// filter logic directly by building std::vector<SceneMarker> in the
// test body. The free function filterMarkersByType() was split out
// of SceneLoader specifically so tests can hit it without Vulkan.
//
// SceneLoader::getSpawnPoints() on a default-constructed loader is
// also covered — it exercises the wire-through path and proves the
// "no markers → empty" case end-to-end.

#include "SceneLoader.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using sv::SceneMarker;
using sv::SceneLoader;
using sv::filterMarkersByType;

namespace {

// Build a small set of markers covering multiple types for the filter
// tests. Ordering is preserved by the filter so tests that care about
// order can rely on this layout.
std::vector<SceneMarker> makeFixtureMarkers() {
    std::vector<SceneMarker> m(5);

    m[0].name = "spawn_player";
    m[0].type = "spawn_point";
    m[0].position = { 0.0f, 0.0f, 0.0f };

    m[1].name = "spawn_enemy_a";
    m[1].type = "spawn_point";
    m[1].position = { 10.0f, 0.0f, 0.0f };

    m[2].name = "level_exit";
    m[2].type = "trigger";
    m[2].position = { 0.0f, 0.0f, 50.0f };

    m[3].name = "ambient_cave";
    m[3].type = "audio_zone";
    m[3].position = { -5.0f, 2.0f, -5.0f };

    m[4].name = "spawn_enemy_b";
    m[4].type = "spawn_point";
    m[4].position = { -10.0f, 0.0f, 0.0f };

    return m;
}

} // anonymous

// ── filterMarkersByType free function ───────────────────────────────

TEST_CASE("filterMarkersByType: empty input yields empty result",
          "[scene-loader][spawn]") {
    std::vector<SceneMarker> empty;
    auto result = filterMarkersByType(empty, "spawn_point");
    REQUIRE(result.empty());
}

TEST_CASE("filterMarkersByType: returns only spawn_point markers",
          "[scene-loader][spawn]") {
    auto markers = makeFixtureMarkers();
    auto spawns  = filterMarkersByType(markers, "spawn_point");

    // Fixture has 3 spawn_point markers
    REQUIRE(spawns.size() == 3);
    for (const SceneMarker* mk : spawns) {
        REQUIRE(mk != nullptr);
        REQUIRE(mk->type == "spawn_point");
    }
}

TEST_CASE("filterMarkersByType: preserves input order",
          "[scene-loader][spawn]") {
    auto markers = makeFixtureMarkers();
    auto spawns  = filterMarkersByType(markers, "spawn_point");

    REQUIRE(spawns.size() == 3);
    REQUIRE(spawns[0]->name == "spawn_player");
    REQUIRE(spawns[1]->name == "spawn_enemy_a");
    REQUIRE(spawns[2]->name == "spawn_enemy_b");
}

TEST_CASE("filterMarkersByType: other marker types filtered separately",
          "[scene-loader][spawn]") {
    auto markers  = makeFixtureMarkers();

    auto triggers = filterMarkersByType(markers, "trigger");
    REQUIRE(triggers.size() == 1);
    REQUIRE(triggers[0]->name == "level_exit");

    auto audio = filterMarkersByType(markers, "audio_zone");
    REQUIRE(audio.size() == 1);
    REQUIRE(audio[0]->name == "ambient_cave");
}

TEST_CASE("filterMarkersByType: unknown type yields empty",
          "[scene-loader][spawn]") {
    auto markers = makeFixtureMarkers();
    auto result  = filterMarkersByType(markers, "nonexistent_type");
    REQUIRE(result.empty());
}

TEST_CASE("filterMarkersByType: empty string matches markers with empty type",
          "[scene-loader][spawn]") {
    std::vector<SceneMarker> markers(2);
    markers[0].name = "untyped_a";
    markers[0].type = ""; // no type
    markers[1].name = "typed";
    markers[1].type = "spawn_point";

    auto result = filterMarkersByType(markers, "");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0]->name == "untyped_a");
}

TEST_CASE("filterMarkersByType: returned pointers alias input vector",
          "[scene-loader][spawn]") {
    auto markers = makeFixtureMarkers();
    auto result  = filterMarkersByType(markers, "spawn_point");

    // First spawn should point at markers[0]
    REQUIRE(result[0] == &markers[0]);
    // Position should match the source
    REQUIRE(result[0]->position.x == 0.0f);
    REQUIRE(result[2] == &markers[4]);
}

// ── SceneLoader::getSpawnPoints wire-through ────────────────────────

TEST_CASE("SceneLoader::getSpawnPoints: default-constructed loader yields empty",
          "[scene-loader][spawn]") {
    SceneLoader loader;
    auto spawns = loader.getSpawnPoints();         // defaults to "spawn_point"
    REQUIRE(spawns.empty());

    auto triggers = loader.getSpawnPoints("trigger");
    REQUIRE(triggers.empty());
}
