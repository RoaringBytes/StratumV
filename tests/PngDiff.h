// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── S-T-RENDER1: Golden-image PNG diff primitive ─────────────────
// Shared helper for render-regression tests. Loads two PNGs via
// stb_image (already linked by stratumv.lib through tinygltf), runs
// a per-pixel comparison, and returns both a root-mean-square error
// and a maximum absolute channel difference. Tests pick whichever
// threshold fits the pass being checked — tight passes (e.g. the
// rest-pose mesh bake) can demand RMS < 1.0, while floatier passes
// (e.g. CPU tonemap reference) can allow larger drift.
//
// Both inputs MUST have matching dimensions and matching channel
// counts. The caller is responsible for committing the "golden"
// side and regenerating it intentionally after an approved change.
//
// Design choices:
//  - Header-only: keeps the test target from growing a dedicated
//    translation unit just for a ~50-LOC helper.
//  - No stb_image_write: writing PNGs is a capture-side concern,
//    already handled by the lab harness via DevServer.cpp's
//    STB_IMAGE_WRITE_IMPLEMENTATION. Tests only READ.
//  - Public namespace `svtest` mirrors test_util.h — keeps the
//    Catch2 binary's symbols visually separated from `sv::`.
#pragma once

#include <stb_image.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace svtest {

// ── Result of a single PNG diff ─────────────────────────────────
// A `DiffResult` is the full story of one comparison: pixel sizes,
// any per-channel maxima, and an RMS over all channels. Callers
// use the numeric fields for Catch2 REQUIRE() checks; the `error`
// field is non-empty on fatal load failures.
struct DiffResult {
    bool        ok           = false; // false = one of the loads failed
    int         width        = 0;
    int         height       = 0;
    int         channels     = 0;
    double      rms          = 0.0;   // root-mean-square over all channels
    int         maxAbsDiff   = 0;     // largest single-channel |a - b|
    uint64_t    pixelCount   = 0;
    std::string error;                // non-empty on failure
};

// ── loadPng ─────────────────────────────────────────────────────
// Thin stb_image wrapper. Returns the raw uint8 buffer + dimensions
// via out-parameters; caller is responsible for stbi_image_free.
// Force-channels to 4 so that RGBA and RGB goldens diff identically
// — ignoring the spurious difference in alpha for RGB sources.
inline bool loadPng(const std::string& path,
                    std::vector<uint8_t>& out,
                    int& width, int& height)
{
    int w = 0, h = 0, ch = 0;
    stbi_uc* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!data) {
        return false;
    }
    out.assign(data, data + (size_t)w * (size_t)h * 4);
    width  = w;
    height = h;
    stbi_image_free(data);
    return true;
}

// ── diffPng ─────────────────────────────────────────────────────
// Compares two PNG files pixel-by-pixel. Both files are force-loaded
// to RGBA8. Sets `ok=false` with a descriptive error when:
//  - either file fails to load (missing, corrupt, unsupported format)
//  - dimensions differ (indicative of a capture-rig change — fatal)
//
// On success, populates rms, maxAbsDiff, width, height, channels=4,
// and pixelCount (w*h). Channels is always 4 because loadPng forces
// it — this simplifies downstream threshold logic.
inline DiffResult diffPng(const std::string& goldenPath,
                          const std::string& capturedPath)
{
    DiffResult r;

    std::vector<uint8_t> golden, captured;
    int gw = 0, gh = 0, cw = 0, ch = 0;

    if (!loadPng(goldenPath, golden, gw, gh)) {
        r.error = "failed to load golden PNG: " + goldenPath;
        return r;
    }
    if (!loadPng(capturedPath, captured, cw, ch)) {
        r.error = "failed to load captured PNG: " + capturedPath;
        return r;
    }
    if (gw != cw || gh != ch) {
        r.error  = "dimension mismatch: golden ";
        r.error += std::to_string(gw) + "x" + std::to_string(gh);
        r.error += " vs captured ";
        r.error += std::to_string(cw) + "x" + std::to_string(ch);
        return r;
    }

    r.width      = gw;
    r.height     = gh;
    r.channels   = 4;
    r.pixelCount = (uint64_t)gw * (uint64_t)gh;

    // Walk all channels; compute RMS and track the largest gap.
    // Casts to int are safe because (a,b) ∈ [0,255].
    double   sumSq     = 0.0;
    int      maxDelta  = 0;
    const size_t total = golden.size(); // == captured.size() here
    for (size_t i = 0; i < total; ++i) {
        const int d = (int)golden[i] - (int)captured[i];
        const int a = d < 0 ? -d : d;
        if (a > maxDelta) maxDelta = a;
        sumSq += (double)(d * d);
    }

    r.maxAbsDiff = maxDelta;
    r.rms        = total > 0
                     ? std::sqrt(sumSq / (double)total)
                     : 0.0;
    r.ok         = true;
    return r;
}

// ── formatResult ────────────────────────────────────────────────
// Human-friendly summary used in Catch2 INFO() messages so that a
// failing diff prints dimensions + rms + maxAbsDiff instead of a
// bare boolean. Kept out-of-line-style as a free function so the
// struct definition stays minimal.
inline std::string formatResult(const DiffResult& r)
{
    if (!r.ok) return "diff failed: " + r.error;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%dx%d ch=%d rms=%.3f maxAbs=%d pixels=%llu",
                  r.width, r.height, r.channels,
                  r.rms, r.maxAbsDiff,
                  (unsigned long long)r.pixelCount);
    return buf;
}

} // namespace svtest
