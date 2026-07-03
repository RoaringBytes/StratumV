// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
//
// Unit tests for WorldStateIO: JSON round-trips for TimeOfDayAnchor,
// WeatherOverride, and BiomeOverride, plus defaulting behavior for
// missing keys, partial vec3 arrays, and wrong-typed values.

#include <catch2/catch_test_macros.hpp>

#include "WorldStateIO.h"

using nlohmann::json;

namespace {

bool v3eq(const glm::vec3& a, const glm::vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

} // namespace

// Anchor round-trip

TEST_CASE("WorldStateIO: anchor round-trip preserves modified fields",
          "[worldstateio][anchor]") {
    sv::TimeOfDayAnchor a;
    a.solarElevation   = -12.5f;
    a.solarAzimuth     = 42.0f;
    a.sunColor         = {0.1f, 0.2f, 0.3f};
    a.sunIntensity     = 2.25f;
    a.skyZenith        = {0.9f, 0.8f, 0.7f};
    a.cloudDensity     = 0.125f;
    a.cloudFadeEnd     = 0.5f;
    a.godRayCount      = 7.0f;
    a.sunsetWarmStrength = 0.75f;
    a.fogDensity       = 0.03f;
    a.scatterColor     = {0.4f, 0.5f, 0.6f};
    a.hazeStrength     = 0.5f;
    a.shadowIntensity  = 0.25f;
    a.moonPhase        = 0.5f;
    a.starBrightness   = 0.9f;

    const sv::TimeOfDayAnchor b = sv::deserializeAnchor(sv::serializeAnchor(a));

    CHECK(b.solarElevation == a.solarElevation);
    CHECK(b.solarAzimuth   == a.solarAzimuth);
    CHECK(v3eq(b.sunColor, a.sunColor));
    CHECK(b.sunIntensity   == a.sunIntensity);
    CHECK(v3eq(b.skyZenith, a.skyZenith));
    CHECK(b.cloudDensity   == a.cloudDensity);
    CHECK(b.cloudFadeEnd   == a.cloudFadeEnd);
    CHECK(b.godRayCount    == a.godRayCount);
    CHECK(b.sunsetWarmStrength == a.sunsetWarmStrength);
    CHECK(b.fogDensity     == a.fogDensity);
    CHECK(v3eq(b.scatterColor, a.scatterColor));
    CHECK(b.hazeStrength   == a.hazeStrength);
    CHECK(b.shadowIntensity == a.shadowIntensity);
    CHECK(b.moonPhase      == a.moonPhase);
    CHECK(b.starBrightness == a.starBrightness);

    // Untouched fields keep their defaults through the round-trip.
    const sv::TimeOfDayAnchor d;
    CHECK(b.sunDiskSize == d.sunDiskSize);
    CHECK(v3eq(b.cloudBrightColor, d.cloudBrightColor));
    CHECK(b.causticDepth == d.causticDepth);
}

TEST_CASE("WorldStateIO: anchor deserialized from empty JSON is default",
          "[worldstateio][anchor]") {
    const sv::TimeOfDayAnchor d;
    const sv::TimeOfDayAnchor b = sv::deserializeAnchor(json::object());

    CHECK(b.solarElevation == d.solarElevation);
    CHECK(v3eq(b.sunColor, d.sunColor));
    CHECK(v3eq(b.skyHorizon, d.skyHorizon));
    CHECK(b.cloudScale1 == d.cloudScale1);
    CHECK(b.godRayFalloff == d.godRayFalloff);
    CHECK(b.fogEnd == d.fogEnd);
    CHECK(b.sparklePower == d.sparklePower);
    CHECK(b.starTwinkleAmount == d.starTwinkleAmount);
}

TEST_CASE("WorldStateIO: anchor tolerates partial and wrong-typed vec3",
          "[worldstateio][anchor]") {
    const sv::TimeOfDayAnchor d;

    json j;
    j["sun"]["color"]  = json::array({0.5f});      // partial: y/z fall back
    j["sky"]["zenith"] = "not-an-array";           // wrong type: ignored
    const sv::TimeOfDayAnchor b = sv::deserializeAnchor(j);

    CHECK(b.sunColor.x == 0.5f);
    CHECK(b.sunColor.y == d.sunColor.y);
    CHECK(b.sunColor.z == d.sunColor.z);
    CHECK(v3eq(b.skyZenith, d.skyZenith));
}

// Weather round-trip

TEST_CASE("WorldStateIO: weather serializes only enabled properties",
          "[worldstateio][weather]") {
    sv::WeatherOverride w;
    w.transitionTime = 2.5f;
    w.cloudDensity.enabled = true;
    w.cloudDensity.value   = 0.9f;
    w.windSpeed.enabled    = true;
    w.windSpeed.value      = 14.0f;

    const json j = sv::serializeWeather(w);

    CHECK(j["transitionTime"] == 2.5f);
    REQUIRE(j.contains("clouds"));
    CHECK(j["clouds"].contains("density"));
    CHECK_FALSE(j["clouds"].contains("speed"));    // disabled: omitted
    REQUIRE(j.contains("ocean"));
    CHECK(j["ocean"].contains("windSpeed"));
    CHECK_FALSE(j.contains("fog"));                // whole section empty
    CHECK_FALSE(j.contains("atmosphere"));
    CHECK_FALSE(j.contains("godRays"));
}

TEST_CASE("WorldStateIO: weather round-trip restores enabled flags",
          "[worldstateio][weather]") {
    sv::WeatherOverride w;
    w.transitionTime = 8.0f;
    w.cloudSoftness.enabled  = true;  w.cloudSoftness.value  = 0.7f;
    w.fogDensity.enabled     = true;  w.fogDensity.value     = 0.02f;
    w.skyHaze.enabled        = true;  w.skyHaze.value        = 0.6f;
    w.godRayIntensity.enabled = true; w.godRayIntensity.value = 1.5f;

    const sv::WeatherOverride r = sv::deserializeWeather(sv::serializeWeather(w));

    CHECK(r.transitionTime == 8.0f);
    CHECK(r.cloudSoftness.enabled);
    CHECK(r.cloudSoftness.value == 0.7f);
    CHECK(r.fogDensity.enabled);
    CHECK(r.fogDensity.value == 0.02f);
    CHECK(r.skyHaze.enabled);
    CHECK(r.skyHaze.value == 0.6f);
    CHECK(r.godRayIntensity.enabled);
    CHECK(r.godRayIntensity.value == 1.5f);

    // Never-enabled properties stay disabled after the round-trip.
    CHECK_FALSE(r.cloudDensity.enabled);
    CHECK_FALSE(r.windSpeed.enabled);
    CHECK_FALSE(r.fogStart.enabled);
    CHECK_FALSE(r.saturationBoost.enabled);
}

TEST_CASE("WorldStateIO: weather from empty JSON is fully disabled",
          "[worldstateio][weather]") {
    const sv::WeatherOverride r = sv::deserializeWeather(json::object());
    CHECK(r.transitionTime == 5.0f);   // documented default
    CHECK_FALSE(r.cloudDensity.enabled);
    CHECK_FALSE(r.amplitude.enabled);
    CHECK_FALSE(r.fogEnd.enabled);
    CHECK_FALSE(r.godRayIntensity.enabled);
}

// Biome round-trip

TEST_CASE("WorldStateIO: biome round-trip with vec3 tint",
          "[worldstateio][biome]") {
    sv::BiomeOverride b;
    b.transitionTime = 3.0f;
    b.temperature.enabled = true;    b.temperature.value = 31.5f;
    b.waterColorTint.enabled = true; b.waterColorTint.value = {0.8f, 0.9f, 1.0f};

    const json j = sv::serializeBiome(b);
    REQUIRE(j.contains("environment"));
    CHECK_FALSE(j["environment"].contains("humidity"));   // disabled: omitted
    REQUIRE(j.contains("water"));

    const sv::BiomeOverride r = sv::deserializeBiome(j);
    CHECK(r.transitionTime == 3.0f);
    CHECK(r.temperature.enabled);
    CHECK(r.temperature.value == 31.5f);
    CHECK(r.waterColorTint.enabled);
    CHECK(v3eq(r.waterColorTint.value, {0.8f, 0.9f, 1.0f}));
    CHECK_FALSE(r.humidity.enabled);
    CHECK_FALSE(r.vegetationDensity.enabled);
}

TEST_CASE("WorldStateIO: biome vec3 property with partial array",
          "[worldstateio][biome]") {
    json j;
    j["water"]["colorTint"] = json::array({0.25f, 0.5f});
    const sv::BiomeOverride r = sv::deserializeBiome(j);

    CHECK(r.waterColorTint.enabled);
    CHECK(r.waterColorTint.value.x == 0.25f);
    CHECK(r.waterColorTint.value.y == 0.5f);
    CHECK(r.waterColorTint.value.z == 0.0f);   // missing component reads 0

    // Empty biome JSON leaves everything disabled.
    const sv::BiomeOverride e = sv::deserializeBiome(json::object());
    CHECK(e.transitionTime == 5.0f);
    CHECK_FALSE(e.waterColorTint.enabled);
}
