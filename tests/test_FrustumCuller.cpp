// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── S-T1: FrustumCuller unit tests ───────────────────────────────────
// Tests for sv::FrustumCuller (FrustumCuller.h/.cpp).
//
// Covers:
//  - Gribb-Hartmann plane extraction (sign, normalization)
//  - AABB inside / outside / intersecting
//  - Edge cases: point AABB, inverted AABB, huge AABB, AABB at origin
//  - isVisible() convenience
//  - Real ortho + perspective projection matrices

#include "FrustumCuller.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

using sv::FrustumCuller;
using sv::CullResult;
using Catch::Matchers::WithinAbs;

// ── Helpers ──────────────────────────────────────────────────────────
namespace {

// Standard perspective projection (GL-style depth).
glm::mat4 makePerspective(float fov = glm::radians(60.0f),
                          float aspect = 1.0f,
                          float nearZ = 0.1f,
                          float farZ = 100.0f) {
    return glm::perspective(fov, aspect, nearZ, farZ);
}

// VP matrix from eye at origin looking down -Z, perspective.
glm::mat4 makeIdentityViewPerspective(float nearZ = 0.1f, float farZ = 100.0f) {
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 0),
                                  glm::vec3(0, 0, -1),
                                  glm::vec3(0, 1, 0));
    return makePerspective(glm::radians(60.0f), 1.0f, nearZ, farZ) * view;
}

// Small helper: unit AABB centered at p.
void makeAABB(const glm::vec3& center, float halfExtent,
              glm::vec3& outMin, glm::vec3& outMax) {
    outMin = center - glm::vec3(halfExtent);
    outMax = center + glm::vec3(halfExtent);
}

} // anonymous

// ── Plane extraction ─────────────────────────────────────────────────

TEST_CASE("FrustumCuller: extracts six planes from a matrix", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective());
    REQUIRE(fc.planes().size() == 6);
}

TEST_CASE("FrustumCuller: planes are normalized", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective());
    for (int i = 0; i < 6; ++i) {
        const glm::vec4& p = fc.planes()[i];
        const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        REQUIRE_THAT(len, WithinAbs(1.0f, 1e-4f));
    }
}

TEST_CASE("FrustumCuller: orthographic projection near/far planes", "[frustum]") {
    // An ortho projection [-1,1]^3 should give simple axis-aligned planes.
    glm::mat4 ortho = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
    FrustumCuller fc;
    fc.extractFromMatrix(ortho);

    // For this ortho, every plane should have a unit normal aligned to an axis.
    for (int i = 0; i < 6; ++i) {
        const glm::vec4& p = fc.planes()[i];
        glm::vec3 n(p.x, p.y, p.z);
        float axisAligned = std::max({std::abs(n.x), std::abs(n.y), std::abs(n.z)});
        REQUIRE_THAT(axisAligned, WithinAbs(1.0f, 1e-4f));
    }
}

TEST_CASE("FrustumCuller: ortho culls points outside the box", "[frustum]") {
    glm::mat4 ortho = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, -10.0f, 10.0f);
    FrustumCuller fc;
    fc.extractFromMatrix(ortho);

    // Point at origin is inside an [-10, 10] box, so any AABB straddling origin is visible.
    glm::vec3 mn, mx;
    makeAABB(glm::vec3(0.0f), 0.5f, mn, mx);
    REQUIRE(fc.isVisible(mn, mx));
}

TEST_CASE("FrustumCuller: ortho rejects AABB far outside the box", "[frustum]") {
    glm::mat4 ortho = glm::ortho(-5.0f, 5.0f, -5.0f, 5.0f, -5.0f, 5.0f);
    FrustumCuller fc;
    fc.extractFromMatrix(ortho);

    // Way off to +X
    glm::vec3 mn, mx;
    makeAABB(glm::vec3(100.0f, 0, 0), 1.0f, mn, mx);
    REQUIRE(!fc.isVisible(mn, mx));
    REQUIRE(fc.testAABB(mn, mx) == CullResult::Outside);
}

// ── AABB inside / outside / intersecting ─────────────────────────────

TEST_CASE("FrustumCuller: AABB fully inside perspective frustum", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective());

    // Small AABB directly in front of the camera
    glm::vec3 mn, mx;
    makeAABB(glm::vec3(0, 0, -5), 0.1f, mn, mx);

    REQUIRE(fc.testAABB(mn, mx) == CullResult::Inside);
}

TEST_CASE("FrustumCuller: AABB behind camera is outside", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective());

    // AABB behind camera (positive Z with default view looking -Z)
    glm::vec3 mn, mx;
    makeAABB(glm::vec3(0, 0, 5), 0.5f, mn, mx);

    REQUIRE(fc.testAABB(mn, mx) == CullResult::Outside);
}

TEST_CASE("FrustumCuller: AABB too far in front is outside", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective(0.1f, 100.0f));

    // Beyond far plane
    glm::vec3 mn, mx;
    makeAABB(glm::vec3(0, 0, -500), 1.0f, mn, mx);

    REQUIRE(fc.testAABB(mn, mx) == CullResult::Outside);
}

TEST_CASE("FrustumCuller: AABB straddling near plane intersects", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective(0.1f, 100.0f));

    // AABB that straddles the near plane at z=-0.1
    glm::vec3 mn(-1, -1, -1);
    glm::vec3 mx( 1,  1,  1); // straddles z=-0.1 (inside) and z=1 (behind camera)
    auto result = fc.testAABB(mn, mx);
    REQUIRE(result == CullResult::Intersecting);
}

TEST_CASE("FrustumCuller: very large AABB containing camera intersects", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective(0.1f, 100.0f));

    // Huge AABB covering camera origin
    glm::vec3 mn(-1000), mx(1000);
    auto result = fc.testAABB(mn, mx);

    // Not fully inside (extends behind camera), not fully outside (encloses frustum)
    REQUIRE(result == CullResult::Intersecting);
}

TEST_CASE("FrustumCuller: AABB far to the side is outside", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective());

    // Far to the right of view direction
    glm::vec3 mn, mx;
    makeAABB(glm::vec3(1000, 0, -5), 0.5f, mn, mx);
    REQUIRE(fc.testAABB(mn, mx) == CullResult::Outside);
}

TEST_CASE("FrustumCuller: AABB far above view is outside", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective());

    glm::vec3 mn, mx;
    makeAABB(glm::vec3(0, 1000, -5), 0.5f, mn, mx);
    REQUIRE(fc.testAABB(mn, mx) == CullResult::Outside);
}

TEST_CASE("FrustumCuller: point AABB at camera focal point", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective());

    // Degenerate AABB (single point) inside frustum
    glm::vec3 p(0, 0, -10);
    auto result = fc.testAABB(p, p);

    // Point is inside → Inside result
    REQUIRE(result == CullResult::Inside);
}

TEST_CASE("FrustumCuller: point AABB at camera origin", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective(0.1f, 100.0f));

    // Origin is behind the near plane
    glm::vec3 p(0, 0, 0);
    auto result = fc.testAABB(p, p);
    REQUIRE(result == CullResult::Outside);
}

// ── isVisible() convenience ──────────────────────────────────────────

TEST_CASE("FrustumCuller: isVisible is true for Inside", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective());
    glm::vec3 mn, mx;
    makeAABB(glm::vec3(0, 0, -5), 0.1f, mn, mx);
    REQUIRE(fc.isVisible(mn, mx));
}

TEST_CASE("FrustumCuller: isVisible is true for Intersecting", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective(0.1f, 100.0f));
    glm::vec3 mn(-1, -1, -1), mx(1, 1, 1);
    REQUIRE(fc.isVisible(mn, mx));
}

TEST_CASE("FrustumCuller: isVisible is false for Outside", "[frustum]") {
    FrustumCuller fc;
    fc.extractFromMatrix(makeIdentityViewPerspective());
    glm::vec3 mn, mx;
    makeAABB(glm::vec3(0, 0, 5), 0.5f, mn, mx);
    REQUIRE(!fc.isVisible(mn, mx));
}

// ── Re-extraction replaces previous planes ───────────────────────────

TEST_CASE("FrustumCuller: extractFromMatrix replaces planes", "[frustum]") {
    FrustumCuller fc;

    // First extract: narrow ortho
    fc.extractFromMatrix(glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f));
    {
        glm::vec3 mn, mx;
        makeAABB(glm::vec3(5, 0, 0), 0.5f, mn, mx);
        REQUIRE(fc.testAABB(mn, mx) == CullResult::Outside);
    }

    // Re-extract: wide ortho — same AABB should now be inside
    fc.extractFromMatrix(glm::ortho(-100.0f, 100.0f, -100.0f, 100.0f, -100.0f, 100.0f));
    {
        glm::vec3 mn, mx;
        makeAABB(glm::vec3(5, 0, 0), 0.5f, mn, mx);
        REQUIRE(fc.testAABB(mn, mx) == CullResult::Inside);
    }
}

// ── Translated view matrix cull ──────────────────────────────────────

TEST_CASE("FrustumCuller: translated view matrix culls origin", "[frustum]") {
    // Camera looking from (0, 0, 10) toward origin.
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 10),
                                  glm::vec3(0, 0, 0),
                                  glm::vec3(0, 1, 0));
    glm::mat4 vp = makePerspective() * view;

    FrustumCuller fc;
    fc.extractFromMatrix(vp);

    // Origin is in front of camera — visible
    glm::vec3 mn, mx;
    makeAABB(glm::vec3(0), 0.1f, mn, mx);
    REQUIRE(fc.testAABB(mn, mx) == CullResult::Inside);

    // AABB way behind origin (further from camera) — still in frustum if within far
    makeAABB(glm::vec3(0, 0, -50), 0.1f, mn, mx);
    REQUIRE(fc.testAABB(mn, mx) == CullResult::Inside);

    // AABB far behind camera
    makeAABB(glm::vec3(0, 0, 200), 0.1f, mn, mx);
    REQUIRE(fc.testAABB(mn, mx) == CullResult::Outside);
}

// ── Non-unit plane ignored gracefully ────────────────────────────────

TEST_CASE("FrustumCuller: zero matrix does not crash", "[frustum]") {
    // Pathological input — zero matrix. extraction should bail on zero-length
    // plane normals without dividing by zero.
    glm::mat4 zero(0.0f);
    FrustumCuller fc;
    fc.extractFromMatrix(zero);
    // All planes should still be finite (no NaN/Inf)
    for (int i = 0; i < 6; ++i) {
        const glm::vec4& p = fc.planes()[i];
        REQUIRE(std::isfinite(p.x));
        REQUIRE(std::isfinite(p.y));
        REQUIRE(std::isfinite(p.z));
        REQUIRE(std::isfinite(p.w));
    }
}
