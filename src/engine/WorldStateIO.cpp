// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "WorldStateIO.h"

namespace sv {

// ── Helpers ─────────────────────────────────────────────────────────

static nlohmann::json jv3(const glm::vec3& v) { return {v.x, v.y, v.z}; }

static glm::vec3 gv3(const nlohmann::json& j, const char* key, const glm::vec3& def)
{
    if (!j.contains(key) || !j[key].is_array()) return def;
    auto& a = j[key];
    return glm::vec3(
        a.size() > 0 ? a[0].get<float>() : def.x,
        a.size() > 1 ? a[1].get<float>() : def.y,
        a.size() > 2 ? a[2].get<float>() : def.z
    );
}

static float gf(const nlohmann::json& j, const char* key, float def)
{
    return j.value(key, def);
}

// ── Serialize Anchor ────────────────────────────────────────────────

nlohmann::json serializeAnchor(const TimeOfDayAnchor& a)
{
    return {
        {"solar", {
            {"elevation",  a.solarElevation},
            {"azimuth",    a.solarAzimuth}
        }},
        {"sun", {
            {"color",      jv3(a.sunColor)},
            {"intensity",  a.sunIntensity},
            {"ambient",    jv3(a.ambientColor)}
        }},
        {"sky", {
            {"zenith",           jv3(a.skyZenith)},
            {"horizon",          jv3(a.skyHorizon)},
            {"mid",              jv3(a.skyMid)},
            {"nadir",            jv3(a.nadirColor)},
            {"wideGlowColor",    jv3(a.wideGlowColor)},
            {"wideGlowStrength", a.wideGlowStrength},
            {"sunGlowPower",     a.sunGlowPower},
            {"sunDiskSize",      a.sunDiskSize},
            {"sunGlowMultiplier", a.sunGlowMultiplier},
            {"wideGlowExponent", a.wideGlowExponent},
            {"haze",             a.skyHaze},
            {"saturationBoost",  a.saturationBoost},
            {"gradientExponent", a.skyGradientExponent},
            {"gradientSplit",    a.skyGradientSplit}
        }},
        {"clouds", {
            {"brightColor",   jv3(a.cloudBrightColor)},
            {"shadowColor",   jv3(a.cloudShadowColor)},
            {"glowColor",     jv3(a.cloudGlowColor)},
            {"density",       a.cloudDensity},
            {"speed",         a.cloudSpeed},
            {"height1",       a.cloudHeight1},
            {"scale1",        a.cloudScale1},
            {"detail1",       a.cloudDetail1},
            {"softness",      a.cloudSoftness},
            {"height2",       a.cloudHeight2},
            {"scale2",        a.cloudScale2},
            {"detail2",       a.cloudDetail2},
            {"opacity2",      a.cloudOpacity2},
            {"layerBlend1",   a.cloudLayerBlend1},
            {"layerBlend2",   a.cloudLayerBlend2},
            {"fadeStart",     a.cloudFadeStart},
            {"fadeEnd",       a.cloudFadeEnd},
            {"sunEdgeExp",    a.cloudSunEdgeExp},
            {"minLighting",   a.cloudMinLighting}
        }},
        {"godRays", {
            {"color",      jv3(a.godRayColor)},
            {"intensity",  a.godRayIntensity},
            {"density",    a.godRayDensity},
            {"falloff",    a.godRayFalloff},
            {"multiplier", a.godRayMultiplier},
            {"spread",     a.godRaySpread},
            {"length",     a.godRayLength},
            {"width",      a.godRayWidth},
            {"count",      a.godRayCount},
            {"asymmetry",  a.godRayAsymmetry}
        }},
        {"atmosphere", {
            {"sunsetWarmColor",   jv3(a.sunsetWarmColor)},
            {"sunsetPinkColor",   jv3(a.sunsetPinkColor)},
            {"hazeColorDay",      jv3(a.hazeColorDay)},
            {"hazeColorSunset",   jv3(a.hazeColorSunset)},
            {"sunsetStartHeight", a.sunsetStartHeight},
            {"sunsetWarmStrength", a.sunsetWarmStrength},
            {"sunsetPinkStrength", a.sunsetPinkStrength},
            {"nightDarkness",     a.nightDarkness},
            {"hazeFalloff",       a.hazeFalloff}
        }},
        {"fog", {
            {"color",    jv3(a.fogColor)},
            {"density",  a.fogDensity},
            {"start",    a.fogStart},
            {"end",      a.fogEnd}
        }},
        {"water", {
            {"scatterColor",    jv3(a.scatterColor)},
            {"deepColor",       jv3(a.deepColor)},
            {"sssColor",        jv3(a.sssColor)},
            {"skyReflectLow",   jv3(a.skyReflectLow)},
            {"skyReflectHigh",  jv3(a.skyReflectHigh)},
            {"sunReflectSharp", jv3(a.sunReflectSharp)},
            {"sunReflectBroad", jv3(a.sunReflectBroad)},
            {"hazeColor",       jv3(a.hazeColor_water)},
            {"foamTint",        jv3(a.foamTint)},
            {"sssStrength",        a.sssStrength},
            {"refractionStrength", a.refractionStrength},
            {"causticIntensity",   a.causticIntensity},
            {"causticDepth",       a.causticDepth},
            {"sparkleIntensity",   a.sparkleIntensity},
            {"sparklePower",       a.sparklePower},
            {"hazeStrength",       a.hazeStrength}
        }},
        {"shadow", {
            {"intensity", a.shadowIntensity}
        }},
        {"nightSky", {
            {"moonElevation",     a.moonElevation},
            {"moonAzimuth",       a.moonAzimuth},
            {"moonPhase",         a.moonPhase},
            {"starBrightness",    a.starBrightness},
            {"starDensity",       a.starDensity},
            {"milkyWayStrength",  a.milkyWayStrength},
            {"starTwinkleAmount", a.starTwinkleAmount}
        }}
    };
}

// ── Deserialize Anchor ──────────────────────────────────────────────

TimeOfDayAnchor deserializeAnchor(const nlohmann::json& j)
{
    TimeOfDayAnchor a;

    if (j.contains("solar")) {
        auto& s = j["solar"];
        a.solarElevation = gf(s, "elevation", a.solarElevation);
        a.solarAzimuth   = gf(s, "azimuth",   a.solarAzimuth);
    }
    if (j.contains("sun")) {
        auto& s = j["sun"];
        a.sunColor     = gv3(s, "color",   a.sunColor);
        a.sunIntensity = gf(s, "intensity", a.sunIntensity);
        a.ambientColor = gv3(s, "ambient", a.ambientColor);
    }
    if (j.contains("sky")) {
        auto& s = j["sky"];
        a.skyZenith         = gv3(s, "zenith",   a.skyZenith);
        a.skyHorizon        = gv3(s, "horizon",  a.skyHorizon);
        a.skyMid            = gv3(s, "mid",      a.skyMid);
        a.nadirColor        = gv3(s, "nadir",    a.nadirColor);
        a.wideGlowColor     = gv3(s, "wideGlowColor", a.wideGlowColor);
        a.wideGlowStrength  = gf(s, "wideGlowStrength", a.wideGlowStrength);
        a.sunGlowPower      = gf(s, "sunGlowPower",     a.sunGlowPower);
        a.sunDiskSize       = gf(s, "sunDiskSize",       a.sunDiskSize);
        a.sunGlowMultiplier = gf(s, "sunGlowMultiplier", a.sunGlowMultiplier);
        a.wideGlowExponent  = gf(s, "wideGlowExponent",  a.wideGlowExponent);
        a.skyHaze           = gf(s, "haze",              a.skyHaze);
        a.saturationBoost   = gf(s, "saturationBoost",   a.saturationBoost);
        a.skyGradientExponent = gf(s, "gradientExponent", a.skyGradientExponent);
        a.skyGradientSplit    = gf(s, "gradientSplit",    a.skyGradientSplit);
    }
    if (j.contains("clouds")) {
        auto& c = j["clouds"];
        a.cloudBrightColor = gv3(c, "brightColor", a.cloudBrightColor);
        a.cloudShadowColor = gv3(c, "shadowColor", a.cloudShadowColor);
        a.cloudGlowColor   = gv3(c, "glowColor",   a.cloudGlowColor);
        a.cloudDensity     = gf(c, "density",   a.cloudDensity);
        a.cloudSpeed       = gf(c, "speed",     a.cloudSpeed);
        a.cloudHeight1     = gf(c, "height1",   a.cloudHeight1);
        a.cloudScale1      = gf(c, "scale1",    a.cloudScale1);
        a.cloudDetail1     = gf(c, "detail1",   a.cloudDetail1);
        a.cloudSoftness    = gf(c, "softness",  a.cloudSoftness);
        a.cloudHeight2     = gf(c, "height2",   a.cloudHeight2);
        a.cloudScale2      = gf(c, "scale2",    a.cloudScale2);
        a.cloudDetail2     = gf(c, "detail2",   a.cloudDetail2);
        a.cloudOpacity2    = gf(c, "opacity2",  a.cloudOpacity2);
        a.cloudLayerBlend1 = gf(c, "layerBlend1", a.cloudLayerBlend1);
        a.cloudLayerBlend2 = gf(c, "layerBlend2", a.cloudLayerBlend2);
        a.cloudFadeStart   = gf(c, "fadeStart", a.cloudFadeStart);
        a.cloudFadeEnd     = gf(c, "fadeEnd",   a.cloudFadeEnd);
        a.cloudSunEdgeExp  = gf(c, "sunEdgeExp", a.cloudSunEdgeExp);
        a.cloudMinLighting = gf(c, "minLighting", a.cloudMinLighting);
    }
    if (j.contains("godRays")) {
        auto& g = j["godRays"];
        a.godRayColor      = gv3(g, "color",      a.godRayColor);
        a.godRayIntensity  = gf(g, "intensity",   a.godRayIntensity);
        a.godRayDensity    = gf(g, "density",     a.godRayDensity);
        a.godRayFalloff    = gf(g, "falloff",     a.godRayFalloff);
        a.godRayMultiplier = gf(g, "multiplier",  a.godRayMultiplier);
        a.godRaySpread     = gf(g, "spread",      a.godRaySpread);
        a.godRayLength     = gf(g, "length",      a.godRayLength);
        a.godRayWidth      = gf(g, "width",       a.godRayWidth);
        a.godRayCount      = gf(g, "count",       a.godRayCount);
        a.godRayAsymmetry  = gf(g, "asymmetry",   a.godRayAsymmetry);
    }
    if (j.contains("atmosphere")) {
        auto& at = j["atmosphere"];
        a.sunsetWarmColor    = gv3(at, "sunsetWarmColor",   a.sunsetWarmColor);
        a.sunsetPinkColor    = gv3(at, "sunsetPinkColor",   a.sunsetPinkColor);
        a.hazeColorDay       = gv3(at, "hazeColorDay",      a.hazeColorDay);
        a.hazeColorSunset    = gv3(at, "hazeColorSunset",   a.hazeColorSunset);
        a.sunsetStartHeight  = gf(at, "sunsetStartHeight",  a.sunsetStartHeight);
        a.sunsetWarmStrength = gf(at, "sunsetWarmStrength",  a.sunsetWarmStrength);
        a.sunsetPinkStrength = gf(at, "sunsetPinkStrength",  a.sunsetPinkStrength);
        a.nightDarkness      = gf(at, "nightDarkness",      a.nightDarkness);
        a.hazeFalloff        = gf(at, "hazeFalloff",        a.hazeFalloff);
    }
    if (j.contains("fog")) {
        auto& f = j["fog"];
        a.fogColor   = gv3(f, "color",   a.fogColor);
        a.fogDensity = gf(f, "density",  a.fogDensity);
        a.fogStart   = gf(f, "start",    a.fogStart);
        a.fogEnd     = gf(f, "end",      a.fogEnd);
    }
    if (j.contains("water")) {
        auto& w = j["water"];
        a.scatterColor      = gv3(w, "scatterColor",    a.scatterColor);
        a.deepColor         = gv3(w, "deepColor",       a.deepColor);
        a.sssColor          = gv3(w, "sssColor",        a.sssColor);
        a.skyReflectLow     = gv3(w, "skyReflectLow",   a.skyReflectLow);
        a.skyReflectHigh    = gv3(w, "skyReflectHigh",  a.skyReflectHigh);
        a.sunReflectSharp   = gv3(w, "sunReflectSharp", a.sunReflectSharp);
        a.sunReflectBroad   = gv3(w, "sunReflectBroad", a.sunReflectBroad);
        a.hazeColor_water   = gv3(w, "hazeColor",       a.hazeColor_water);
        a.foamTint          = gv3(w, "foamTint",        a.foamTint);
        a.sssStrength        = gf(w, "sssStrength",        a.sssStrength);
        a.refractionStrength = gf(w, "refractionStrength", a.refractionStrength);
        a.causticIntensity   = gf(w, "causticIntensity",   a.causticIntensity);
        a.causticDepth       = gf(w, "causticDepth",       a.causticDepth);
        a.sparkleIntensity   = gf(w, "sparkleIntensity",   a.sparkleIntensity);
        a.sparklePower       = gf(w, "sparklePower",       a.sparklePower);
        a.hazeStrength       = gf(w, "hazeStrength",       a.hazeStrength);
    }
    if (j.contains("shadow")) {
        auto& sh = j["shadow"];
        a.shadowIntensity = gf(sh, "intensity", a.shadowIntensity);
    }
    if (j.contains("nightSky")) {
        auto& ns = j["nightSky"];
        a.moonElevation     = gf(ns, "moonElevation",     a.moonElevation);
        a.moonAzimuth       = gf(ns, "moonAzimuth",       a.moonAzimuth);
        a.moonPhase         = gf(ns, "moonPhase",          a.moonPhase);
        a.starBrightness    = gf(ns, "starBrightness",     a.starBrightness);
        a.starDensity       = gf(ns, "starDensity",        a.starDensity);
        a.milkyWayStrength  = gf(ns, "milkyWayStrength",   a.milkyWayStrength);
        a.starTwinkleAmount = gf(ns, "starTwinkleAmount",  a.starTwinkleAmount);
    }

    return a;
}

// ── Weather serialize helpers ───────────────────────────────────────

static void putWSP(nlohmann::json& section, const char* key,
                   const WorldStateProperty<float>& p)
{
    if (p.enabled) section[key] = p.value;
}

static void getWSP(const nlohmann::json& section, const char* key,
                   WorldStateProperty<float>& p)
{
    if (section.contains(key)) {
        p.enabled = true;
        p.value   = section[key].get<float>();
    }
}

// ── Serialize Weather ───────────────────────────────────────────────

nlohmann::json serializeWeather(const WeatherOverride& w)
{
    nlohmann::json j;
    j["transitionTime"] = w.transitionTime;

    nlohmann::json clouds;
    putWSP(clouds, "density",     w.cloudDensity);
    putWSP(clouds, "speed",       w.cloudSpeed);
    putWSP(clouds, "softness",    w.cloudSoftness);
    putWSP(clouds, "opacity2",    w.cloudOpacity2);
    putWSP(clouds, "layerBlend1", w.cloudLayerBlend1);
    putWSP(clouds, "layerBlend2", w.cloudLayerBlend2);
    putWSP(clouds, "minLighting", w.cloudMinLighting);
    if (!clouds.empty()) j["clouds"] = clouds;

    nlohmann::json ocean;
    putWSP(ocean, "windSpeed",              w.windSpeed);
    putWSP(ocean, "amplitude",              w.amplitude);
    putWSP(ocean, "choppiness",             w.choppiness);
    putWSP(ocean, "dampingScale",           w.dampingScale);
    putWSP(ocean, "peakEnhancement",        w.peakEnhancement);
    putWSP(ocean, "swellStrength",          w.swellStrength);
    putWSP(ocean, "foamGenerationStrength", w.foamGenerationStrength);
    if (!ocean.empty()) j["ocean"] = ocean;

    nlohmann::json fog;
    putWSP(fog, "density", w.fogDensity);
    putWSP(fog, "start",   w.fogStart);
    putWSP(fog, "end",     w.fogEnd);
    if (!fog.empty()) j["fog"] = fog;

    nlohmann::json atmo;
    putWSP(atmo, "skyHaze",         w.skyHaze);
    putWSP(atmo, "saturationBoost", w.saturationBoost);
    if (!atmo.empty()) j["atmosphere"] = atmo;

    nlohmann::json gr;
    putWSP(gr, "intensity", w.godRayIntensity);
    if (!gr.empty()) j["godRays"] = gr;

    return j;
}

// ── Deserialize Weather ─────────────────────────────────────────────

WeatherOverride deserializeWeather(const nlohmann::json& j)
{
    WeatherOverride w;
    w.transitionTime = j.value("transitionTime", 5.0f);

    if (j.contains("clouds")) {
        auto& c = j["clouds"];
        getWSP(c, "density",     w.cloudDensity);
        getWSP(c, "speed",       w.cloudSpeed);
        getWSP(c, "softness",    w.cloudSoftness);
        getWSP(c, "opacity2",    w.cloudOpacity2);
        getWSP(c, "layerBlend1", w.cloudLayerBlend1);
        getWSP(c, "layerBlend2", w.cloudLayerBlend2);
        getWSP(c, "minLighting", w.cloudMinLighting);
    }
    if (j.contains("ocean")) {
        auto& o = j["ocean"];
        getWSP(o, "windSpeed",              w.windSpeed);
        getWSP(o, "amplitude",              w.amplitude);
        getWSP(o, "choppiness",             w.choppiness);
        getWSP(o, "dampingScale",           w.dampingScale);
        getWSP(o, "peakEnhancement",        w.peakEnhancement);
        getWSP(o, "swellStrength",          w.swellStrength);
        getWSP(o, "foamGenerationStrength", w.foamGenerationStrength);
    }
    if (j.contains("fog")) {
        auto& f = j["fog"];
        getWSP(f, "density", w.fogDensity);
        getWSP(f, "start",   w.fogStart);
        getWSP(f, "end",     w.fogEnd);
    }
    if (j.contains("atmosphere")) {
        auto& a = j["atmosphere"];
        getWSP(a, "skyHaze",         w.skyHaze);
        getWSP(a, "saturationBoost", w.saturationBoost);
    }
    if (j.contains("godRays")) {
        auto& g = j["godRays"];
        getWSP(g, "intensity", w.godRayIntensity);
    }

    return w;
}

// ── Biome serialize helpers ─────────────────────────────────────────

static void putWSPv3(nlohmann::json& section, const char* key,
                     const WorldStateProperty<glm::vec3>& p)
{
    if (p.enabled) section[key] = jv3(p.value);
}

static void getWSPv3(const nlohmann::json& section, const char* key,
                     WorldStateProperty<glm::vec3>& p)
{
    if (section.contains(key) && section[key].is_array()) {
        p.enabled = true;
        auto& a = section[key];
        p.value = glm::vec3(
            a.size() > 0 ? a[0].get<float>() : 0.f,
            a.size() > 1 ? a[1].get<float>() : 0.f,
            a.size() > 2 ? a[2].get<float>() : 0.f);
    }
}

nlohmann::json serializeBiome(const BiomeOverride& b)
{
    nlohmann::json j;
    j["transitionTime"] = b.transitionTime;

    nlohmann::json env;
    putWSP(env, "temperature",       b.temperature);
    putWSP(env, "humidity",          b.humidity);
    putWSP(env, "vegetationDensity", b.vegetationDensity);
    if (!env.empty()) j["environment"] = env;

    nlohmann::json water;
    putWSPv3(water, "colorTint", b.waterColorTint);
    if (!water.empty()) j["water"] = water;

    return j;
}

BiomeOverride deserializeBiome(const nlohmann::json& j)
{
    BiomeOverride b;
    b.transitionTime = j.value("transitionTime", 5.0f);

    if (j.contains("environment")) {
        auto& e = j["environment"];
        getWSP(e, "temperature",       b.temperature);
        getWSP(e, "humidity",          b.humidity);
        getWSP(e, "vegetationDensity", b.vegetationDensity);
    }
    if (j.contains("water")) {
        auto& w = j["water"];
        getWSPv3(w, "colorTint", b.waterColorTint);
    }

    return b;
}

} // namespace sv
