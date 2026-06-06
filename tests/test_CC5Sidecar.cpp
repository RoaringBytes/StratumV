// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── S-T1: CC5Sidecar unit tests ──────────────────────────────────────
// Tests for sv::parseCC5Sidecar (vk/CC5Sidecar.h, vk/CC5Sidecar.cpp).
//
// The parser takes an FBX path, swaps the extension to .json, and parses the
// CC5 sidecar structure. Tests use fixture JSONs under tests/fixtures/ —
// the test code constructs fake "FBX" paths pointing at the fixtures so that
// parseCC5Sidecar derives the correct .json path.

#include "vk/CC5Sidecar.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>

using sv::parseCC5Sidecar;
using sv::CC5MatMap;
using sv::CC5MatInfo;
using Catch::Matchers::WithinAbs;

namespace {

// parseCC5Sidecar takes an FBX path and converts it to .json by swapping
// the extension. The fixture files live in tests/fixtures/ — we construct a
// fake FBX path so the derived .json path points at the fixture.
std::string fakeFbxPathForFixture(const char* fixtureJsonStem) {
    // e.g. ".../fixtures/cc5_sample.json" -> pass ".../fixtures/cc5_sample.fbx"
    return std::string(SV_TEST_FIXTURES_DIR) + "/" + fixtureJsonStem + ".fbx";
}

} // anonymous

// ── Happy path: full CC5 file with 4 materials ──────────────────────

TEST_CASE("CC5Sidecar: parses all materials from sample fixture", "[cc5]") {
    CC5MatMap map = parseCC5Sidecar(fakeFbxPathForFixture("cc5_sample"));

    // Fixture has 4 materials
    REQUIRE(map.size() == 4);
    REQUIRE(map.count("Std_Skin_Head") == 1);
    REQUIRE(map.count("Std_Skin_Body") == 1);
    REQUIRE(map.count("Hair_Transparent") == 1);
    REQUIRE(map.count("Eyelash_Mat") == 1);
}

TEST_CASE("CC5Sidecar: node type and two-side flags", "[cc5]") {
    CC5MatMap map = parseCC5Sidecar(fakeFbxPathForFixture("cc5_sample"));

    const CC5MatInfo& head = map.at("Std_Skin_Head");
    REQUIRE(head.nodeType.empty());
    REQUIRE(head.twoSide == false);

    const CC5MatInfo& hair = map.at("Hair_Transparent");
    REQUIRE(hair.nodeType == "Hair");
    REQUIRE(hair.twoSide == true);

    const CC5MatInfo& lash = map.at("Eyelash_Mat");
    REQUIRE(lash.nodeType == "Eyelash");
    REQUIRE(lash.twoSide == false);
}

TEST_CASE("CC5Sidecar: detects opacity from Textures section", "[cc5]") {
    CC5MatMap map = parseCC5Sidecar(fakeFbxPathForFixture("cc5_sample"));

    // Skin materials have no opacity
    REQUIRE(map.at("Std_Skin_Head").hasOpacity == false);
    REQUIRE(map.at("Std_Skin_Body").hasOpacity == false);

    // Hair and eyelash have opacity textures
    REQUIRE(map.at("Hair_Transparent").hasOpacity == true);
    REQUIRE(map.at("Eyelash_Mat").hasOpacity == true);
}

TEST_CASE("CC5Sidecar: texture paths populated per material", "[cc5]") {
    CC5MatMap map = parseCC5Sidecar(fakeFbxPathForFixture("cc5_sample"));

    const auto& head = map.at("Std_Skin_Head").texturePaths;
    REQUIRE(head.count("Base Color") == 1);
    REQUIRE(head.at("Base Color") == "textures/Std_Skin_Head_Diffuse.png");
    REQUIRE(head.count("Normal") == 1);
    REQUIRE(head.at("Normal") == "textures/Std_Skin_Head_Normal.png");

    const auto& hair = map.at("Hair_Transparent").texturePaths;
    REQUIRE(hair.count("Base Color") == 1);
    REQUIRE(hair.count("Opacity") == 1);
    REQUIRE(hair.at("Opacity") == "textures/Hair_Opacity.png");
}

TEST_CASE("CC5Sidecar: RootColor + TipColor parsed and scaled to [0,1]", "[cc5]") {
    CC5MatMap map = parseCC5Sidecar(fakeFbxPathForFixture("cc5_sample"));

    const CC5MatInfo& hair = map.at("Hair_Transparent");
    REQUIRE(hair.hasRootColor == true);
    REQUIRE(hair.shaderName == "RLHair");

    // RootColor [85, 42, 20] / 255 = (0.333, 0.165, 0.078)
    REQUIRE_THAT(hair.rootColor.r, WithinAbs(85.0f / 255.0f,  1e-4f));
    REQUIRE_THAT(hair.rootColor.g, WithinAbs(42.0f / 255.0f,  1e-4f));
    REQUIRE_THAT(hair.rootColor.b, WithinAbs(20.0f / 255.0f,  1e-4f));

    REQUIRE(hair.hasTipColor == true);
    REQUIRE_THAT(hair.tipColor.r, WithinAbs(212.0f / 255.0f, 1e-4f));
    REQUIRE_THAT(hair.tipColor.g, WithinAbs(170.0f / 255.0f, 1e-4f));
    REQUIRE_THAT(hair.tipColor.b, WithinAbs(102.0f / 255.0f, 1e-4f));
}

TEST_CASE("CC5Sidecar: materials without Custom Shader have no root/tip color", "[cc5]") {
    CC5MatMap map = parseCC5Sidecar(fakeFbxPathForFixture("cc5_sample"));

    REQUIRE(map.at("Std_Skin_Head").hasRootColor == false);
    REQUIRE(map.at("Std_Skin_Head").hasTipColor  == false);
    REQUIRE(map.at("Std_Skin_Head").shaderName.empty());
}

// ── Missing file / empty fixtures ───────────────────────────────────

TEST_CASE("CC5Sidecar: missing JSON returns empty map", "[cc5]") {
    CC5MatMap map = parseCC5Sidecar(fakeFbxPathForFixture("definitely_not_there"));
    REQUIRE(map.empty());
}

TEST_CASE("CC5Sidecar: empty Object returns empty map", "[cc5]") {
    CC5MatMap map = parseCC5Sidecar(fakeFbxPathForFixture("cc5_empty"));
    REQUIRE(map.empty());
}
