// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── S-T-RENDER1: Render-regression golden-image tests ────────────
// Diffs the four committed goldens under tests/golden/ against
// captures produced by the lab harness (skinned_test) running
// with `--render-golden <dir>`. The CI fixture invokes the harness
// once before the diff tests run, writes four PNGs to
// ${CMAKE_CURRENT_BINARY_DIR}/golden_captures/, and the diff tests
// load + compare via svtest::diffPng.
//
// Thresholds are deliberately LOOSE on first introduction:
//  - RMS_THRESHOLD: small drift is allowed to absorb differences
//    across driver releases / tiny codegen changes to the skinned
//    shader.
//  - MAX_ABS_DIFF_THRESHOLD: catches an individual pixel flipping
//    hard — if a single channel drifts by more than this many
//    steps, something substantial changed.
//
// The first time a golden is regenerated intentionally (e.g. after
// a shader edit), the developer runs:
//   ./skinned_test --render-golden tests/golden/
// from the repo root, manually eyeballs the output, and commits.
// See docs/PERF_BASELINE.md for a parallel "refresh" workflow.
//
// Tests are tagged `[golden]` so they can be run in isolation with:
//   ctest -R render_golden
// and gated behind `STRATUMV_GOLDEN_TESTS=ON` in tests/CMakeLists.txt
// so a default build doesn't depend on the lab harness at all.

#include "PngDiff.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

// ── Thresholds ────────────────────────────────────────────────
// Picked conservatively from the initial capture: every golden
// is byte-identical across two back-to-back runs on the same
// machine, so even very small non-zero values here cushion
// future driver/compiler drift without making the test useless.
constexpr double kRmsThreshold    = 1.5;
constexpr int    kMaxAbsThreshold = 8;

// ── Path resolution ────────────────────────────────────────────
// Goldens: tests/golden/<name>.png (committed). SV_TEST_GOLDEN_DIR
// is set by tests/CMakeLists.txt to ${CMAKE_CURRENT_SOURCE_DIR}/golden.
//
// Captures: the CTest fixture sets SV_GOLDEN_CAPTURE_DIR before
// running sv_tests — we read it from the environment at test time
// so the fixture and the tests can agree on a single directory
// without baking the build path into the binary.
std::string goldenPath(const char* name)
{
    namespace fs = std::filesystem;
#ifdef SV_TEST_GOLDEN_DIR
    return (fs::path(SV_TEST_GOLDEN_DIR) / name).generic_string();
#else
    return std::string("tests/golden/") + name;
#endif
}

std::string capturePath(const char* name)
{
    namespace fs = std::filesystem;
    const char* env = std::getenv("SV_GOLDEN_CAPTURE_DIR");
    if (env && env[0]) {
        return (fs::path(env) / name).generic_string();
    }
    // Fallback: captures landed next to goldens. This only happens
    // when a developer runs the tests from the repo root WITHOUT
    // going through the CTest fixture — i.e. after manually
    // regenerating goldens via `skinned_test --render-golden tests/golden/`.
    return goldenPath(name);
}

// ── diffGolden ──────────────────────────────────────────────────
// Shared body — each named test case just calls this with a
// different PNG. Keeps the Catch2 output informative by including
// the formatted diff result in an INFO() block that fires on any
// REQUIRE() failure.
void diffGolden(const char* name)
{
    const std::string golden   = goldenPath(name);
    const std::string captured = capturePath(name);

    INFO("golden   = " << golden);
    INFO("captured = " << captured);

    const svtest::DiffResult r = svtest::diffPng(golden, captured);
    INFO("result   = " << svtest::formatResult(r));

    REQUIRE(r.ok);
    REQUIRE(r.width  == 512);
    REQUIRE(r.height == 512);
    REQUIRE(r.rms        <= kRmsThreshold);
    REQUIRE(r.maxAbsDiff <= kMaxAbsThreshold);
}

} // namespace

// ── Test cases ──────────────────────────────────────────────────
// One per frozen pass. Name them so that `ctest -R render_golden`
// picks them up predictably, and tag them `[golden]` for filter-
// only runs.
TEST_CASE("render_golden: SkinnedMeshPass bake matches reference",
          "[golden][skinned]")
{
    diffGolden("skinned_mesh.png");
}

TEST_CASE("render_golden: ShadowPass depth readback matches reference",
          "[golden][shadow]")
{
    diffGolden("shadow_pass.png");
}

TEST_CASE("render_golden: PostProcess CPU tonemap matches reference",
          "[golden][postprocess]")
{
    diffGolden("post_process.png");
}

TEST_CASE("render_golden: ImGuiLayer offscreen render matches reference",
          "[golden][imgui]")
{
    diffGolden("imgui_layer.png");
}
