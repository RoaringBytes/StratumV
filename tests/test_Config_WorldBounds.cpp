// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── Config + WorldBounds unit tests ───────────────────────
// Tests for:
//   - sv::WorldBounds default state, valid(), size(), center(), contains()
//   - sv::Config::getVec3 / setVec3 round-trip
//   - sv::Config::worldBounds() on defaults and on custom JSON
//
// Config::loadFromFile() touches the filesystem and logs warnings, so
// we stay in-memory: loadDefaults() + setVec3() + direct JSON
// manipulation via raw().

#include "Config.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/vec3.hpp>

using sv::Config;
using sv::WorldBounds;
using Catch::Matchers::WithinAbs;

namespace {

bool vec3Near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-5f) {
    return std::abs(a.x - b.x) < eps
        && std::abs(a.y - b.y) < eps
        && std::abs(a.z - b.z) < eps;
}

} // anonymous

// ── WorldBounds struct ──────────────────────────────────────────────

TEST_CASE("WorldBounds: default is a huge valid AABB",
          "[config][world-bounds]") {
    WorldBounds b;
    REQUIRE(b.valid());
    REQUIRE(vec3Near(b.min, glm::vec3(-10000.0f)));
    REQUIRE(vec3Near(b.max, glm::vec3( 10000.0f)));
}

TEST_CASE("WorldBounds: valid() rejects degenerate and inverted AABBs",
          "[config][world-bounds]") {
    WorldBounds zero;
    zero.min = glm::vec3(0.0f);
    zero.max = glm::vec3(0.0f);
    REQUIRE_FALSE(zero.valid());

    WorldBounds inverted;
    inverted.min = glm::vec3( 10.0f);
    inverted.max = glm::vec3(-10.0f);
    REQUIRE_FALSE(inverted.valid());

    WorldBounds oneDegenerate;
    oneDegenerate.min = glm::vec3(-1.0f, 0.0f, -1.0f);
    oneDegenerate.max = glm::vec3( 1.0f, 0.0f,  1.0f); // zero Y extent
    REQUIRE_FALSE(oneDegenerate.valid());
}

TEST_CASE("WorldBounds: size and center",
          "[config][world-bounds]") {
    WorldBounds b;
    b.min = glm::vec3(-5.0f, 0.0f, -10.0f);
    b.max = glm::vec3( 5.0f, 8.0f,  10.0f);

    REQUIRE(vec3Near(b.size(),   glm::vec3(10.0f, 8.0f, 20.0f)));
    REQUIRE(vec3Near(b.center(), glm::vec3( 0.0f, 4.0f,  0.0f)));
}

TEST_CASE("WorldBounds: contains is inclusive on all axes",
          "[config][world-bounds]") {
    WorldBounds b;
    b.min = glm::vec3(-1.0f, -1.0f, -1.0f);
    b.max = glm::vec3( 1.0f,  1.0f,  1.0f);

    REQUIRE(b.contains(glm::vec3( 0.0f,  0.0f,  0.0f)));
    REQUIRE(b.contains(glm::vec3( 1.0f,  1.0f,  1.0f)));   // corner
    REQUIRE(b.contains(glm::vec3(-1.0f, -1.0f, -1.0f)));   // opposite corner
    REQUIRE_FALSE(b.contains(glm::vec3( 1.01f, 0.0f, 0.0f)));
    REQUIRE_FALSE(b.contains(glm::vec3( 0.0f, -1.01f, 0.0f)));
}

// ── Config::getVec3 / setVec3 ───────────────────────────────────────

TEST_CASE("Config::getVec3: reads default from loadDefaults",
          "[config][world-bounds]") {
    Config cfg;
    cfg.loadDefaults();

    glm::vec3 boundsMin = cfg.getVec3("world.boundsMin",
                                      glm::vec3(999.0f));
    glm::vec3 boundsMax = cfg.getVec3("world.boundsMax",
                                      glm::vec3(999.0f));
    REQUIRE(vec3Near(boundsMin, glm::vec3(-10000.0f)));
    REQUIRE(vec3Near(boundsMax, glm::vec3( 10000.0f)));
}

TEST_CASE("Config::getVec3: missing path returns fallback",
          "[config][world-bounds]") {
    Config cfg;
    cfg.loadDefaults();

    glm::vec3 result = cfg.getVec3("does.not.exist",
                                   glm::vec3(7.0f, 8.0f, 9.0f));
    REQUIRE(vec3Near(result, glm::vec3(7.0f, 8.0f, 9.0f)));
}

TEST_CASE("Config::setVec3: round-trip via getVec3",
          "[config][world-bounds]") {
    Config cfg;
    cfg.loadDefaults();

    cfg.setVec3("world.boundsMin", glm::vec3(-500.0f, -25.0f, -500.0f));
    cfg.setVec3("world.boundsMax", glm::vec3( 500.0f,  75.0f,  500.0f));

    glm::vec3 lo = cfg.getVec3("world.boundsMin", glm::vec3(0));
    glm::vec3 hi = cfg.getVec3("world.boundsMax", glm::vec3(0));
    REQUIRE(vec3Near(lo, glm::vec3(-500.0f, -25.0f, -500.0f)));
    REQUIRE(vec3Near(hi, glm::vec3( 500.0f,  75.0f,  500.0f)));
}

TEST_CASE("Config::setVec3: creates new nested dot path",
          "[config][world-bounds]") {
    Config cfg; // no defaults loaded → empty JSON

    cfg.setVec3("gameplay.spawnArea.min", glm::vec3(1, 2, 3));
    glm::vec3 result = cfg.getVec3("gameplay.spawnArea.min",
                                   glm::vec3(-1));
    REQUIRE(vec3Near(result, glm::vec3(1, 2, 3)));
}

// ── Config::worldBounds convenience ─────────────────────────────────

TEST_CASE("Config::worldBounds: defaults match WorldBounds default",
          "[config][world-bounds]") {
    Config cfg;
    cfg.loadDefaults();

    WorldBounds b = cfg.worldBounds();
    REQUIRE(b.valid());
    REQUIRE(vec3Near(b.min, glm::vec3(-10000.0f)));
    REQUIRE(vec3Near(b.max, glm::vec3( 10000.0f)));
}

TEST_CASE("Config::worldBounds: reflects custom setVec3 writes",
          "[config][world-bounds]") {
    Config cfg;
    cfg.loadDefaults();

    cfg.setVec3("world.boundsMin", glm::vec3(-64.0f, 0.0f, -64.0f));
    cfg.setVec3("world.boundsMax", glm::vec3( 64.0f, 32.0f, 64.0f));

    WorldBounds b = cfg.worldBounds();
    REQUIRE(vec3Near(b.min, glm::vec3(-64.0f, 0.0f, -64.0f)));
    REQUIRE(vec3Near(b.max, glm::vec3( 64.0f, 32.0f, 64.0f)));
    REQUIRE(b.valid());
    REQUIRE(vec3Near(b.size(),   glm::vec3(128.0f, 32.0f, 128.0f)));
    REQUIRE(vec3Near(b.center(), glm::vec3(  0.0f, 16.0f,   0.0f)));
}

TEST_CASE("Config::setWorldBounds round-trip",
          "[config][world-bounds]") {
    Config cfg;
    cfg.loadDefaults();

    WorldBounds input;
    input.min = glm::vec3(-1.0f, -2.0f, -3.0f);
    input.max = glm::vec3( 4.0f,  5.0f,  6.0f);
    cfg.setWorldBounds(input);

    WorldBounds out = cfg.worldBounds();
    REQUIRE(vec3Near(out.min, input.min));
    REQUIRE(vec3Near(out.max, input.max));
}

TEST_CASE("Config::worldBounds: missing world section returns defaults",
          "[config][world-bounds]") {
    Config cfg; // no loadDefaults() — fully empty

    WorldBounds b = cfg.worldBounds();
    // Both axes should fall back to WorldBounds defaults
    REQUIRE(vec3Near(b.min, glm::vec3(-10000.0f)));
    REQUIRE(vec3Near(b.max, glm::vec3( 10000.0f)));
}
