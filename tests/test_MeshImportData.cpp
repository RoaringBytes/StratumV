// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── MeshImportData unit tests ─────────────────────────────────
// Sanity checks on the CPU-side mesh intermediate produced by 's
// loader split. Tests are intentionally narrow — we verify that default
// construction yields an empty, well-defined state that loaders can fill in.

#include "vk/MeshImportData.h"
#include "vk/VkMesh.h"    // for MeshMaterial / SkeletonData / TextureType enums

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using sv::MeshImportData;
using sv::TextureImportData;
using sv::MeshVertex;
using sv::SubMesh;
using sv::MeshMaterial;
using sv::SkeletonData;
using sv::TextureType;
using sv::BlendMode;
using sv::AABB;
using sv::computeMeshAABB;
using Catch::Matchers::WithinAbs;

TEST_CASE("MeshImportData: default is empty", "[mesh-import]") {
    MeshImportData data;
    REQUIRE(data.vertices.empty());
    REQUIRE(data.indices.empty());
    REQUIRE(data.submeshes.empty());
    REQUIRE(data.materials.empty());
    REQUIRE(data.textures.empty());
    REQUIRE(data.skeleton.empty());
    REQUIRE(data.morphTargetCount == 0);
    REQUIRE(data.morphPosDeltas.empty());
    REQUIRE(data.morphNormDeltas.empty());
    REQUIRE(data.morphTargetNames.empty());
    REQUIRE(data.morphTargetDefaultWeights.empty());
}

TEST_CASE("MeshImportData: vertices can be populated", "[mesh-import]") {
    MeshImportData data;
    MeshVertex v;
    v.pos    = glm::vec3(1, 2, 3);
    v.normal = glm::vec3(0, 1, 0);
    v.uv     = glm::vec2(0.5f, 0.5f);
    data.vertices.push_back(v);

    REQUIRE(data.vertices.size() == 1);
    REQUIRE(data.vertices[0].pos.x == 1.0f);
    REQUIRE(data.vertices[0].pos.y == 2.0f);
    REQUIRE(data.vertices[0].pos.z == 3.0f);
}

TEST_CASE("MeshImportData: submesh index offsets don't overflow", "[mesh-import]") {
    MeshImportData data;
    data.indices.resize(100);
    SubMesh s1{0, 30, 0};
    SubMesh s2{30, 70, 1};
    data.submeshes.push_back(s1);
    data.submeshes.push_back(s2);

    // Simulated invariant: total submesh index count must equal index buffer size
    uint32_t total = 0;
    for (const auto& sm : data.submeshes) total += sm.indexCount;
    REQUIRE(total == data.indices.size());
}

TEST_CASE("TextureImportData: default is empty sRGB baseColor", "[mesh-import]") {
    TextureImportData tex;
    REQUIRE(tex.pixels.empty());
    REQUIRE(tex.width == 0);
    REQUIRE(tex.height == 0);
    REQUIRE(tex.type == TextureType::baseColor);
    REQUIRE(tex.srgb == true);
}

TEST_CASE("MeshMaterial: default is opaque neutral", "[mesh-import]") {
    MeshMaterial mat;
    REQUIRE(mat.baseColor.r == 1.0f);
    REQUIRE(mat.baseColor.g == 1.0f);
    REQUIRE(mat.baseColor.b == 1.0f);
    REQUIRE(mat.baseColor.a == 1.0f);
    REQUIRE(mat.metallic == 0.0f);
    REQUIRE(mat.roughness == 1.0f);
    REQUIRE(mat.baseColorTex == -1);
    REQUIRE(mat.normalTex == -1);
    REQUIRE(mat.blendMode == BlendMode::Opaque);
    REQUIRE(mat.twoSided == false);
}

TEST_CASE("SkeletonData: empty has zero joints", "[mesh-import]") {
    SkeletonData skel;
    REQUIRE(skel.empty());
    REQUIRE(skel.jointCount() == 0);
}

// ── AABB + computeMeshAABB ──────────────────────────────
// Pure CPU math tests — no VkCtx required because computeMeshAABB is
// a free function that takes a std::vector<MeshVertex>. These cover
// the geometry math the lab's fit-to-mesh camera depends on.

namespace {

// Helper to build a MeshVertex with only the position populated.
// Normal/UV/joints/weights are irrelevant to the AABB walk.
MeshVertex vtx(float x, float y, float z)
{
    MeshVertex v;
    v.pos = glm::vec3(x, y, z);
    return v;
}

} // anonymous

TEST_CASE("AABB: default is invalid and zero-sized", "[mesh-import][aabb]") {
    AABB a;
    REQUIRE_FALSE(a.valid);
    REQUIRE(a.min == glm::vec3(0.0f));
    REQUIRE(a.max == glm::vec3(0.0f));
    REQUIRE(a.size() == glm::vec3(0.0f));
    REQUIRE(a.center() == glm::vec3(0.0f));
    REQUIRE_THAT(a.longestAxis(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(a.radius(),      WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("computeMeshAABB: empty vertex list returns invalid AABB",
          "[mesh-import][aabb]") {
    std::vector<MeshVertex> verts;
    AABB a = computeMeshAABB(verts);
    REQUIRE_FALSE(a.valid);
    // Must match default-constructed state so consumers can use
    // `.valid` as the only authoritative "has bounds" signal.
    REQUIRE(a.min == glm::vec3(0.0f));
    REQUIRE(a.max == glm::vec3(0.0f));
}

TEST_CASE("computeMeshAABB: single vertex sets min==max at that point",
          "[mesh-import][aabb]") {
    std::vector<MeshVertex> verts{ vtx(3.0f, -2.0f, 5.0f) };
    AABB a = computeMeshAABB(verts);
    REQUIRE(a.valid);
    REQUIRE(a.min == glm::vec3(3.0f, -2.0f, 5.0f));
    REQUIRE(a.max == glm::vec3(3.0f, -2.0f, 5.0f));
    REQUIRE(a.size() == glm::vec3(0.0f));
    REQUIRE(a.center() == glm::vec3(3.0f, -2.0f, 5.0f));
    REQUIRE_THAT(a.longestAxis(), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("computeMeshAABB: unit cube spanning -1..+1 on all axes",
          "[mesh-import][aabb]") {
    // All 8 corners of a [-1, +1] cube, plus an extra point inside to
    // make sure intermediate verts don't perturb min/max.
    std::vector<MeshVertex> verts{
        vtx(-1, -1, -1), vtx(+1, -1, -1), vtx(-1, +1, -1), vtx(+1, +1, -1),
        vtx(-1, -1, +1), vtx(+1, -1, +1), vtx(-1, +1, +1), vtx(+1, +1, +1),
        vtx( 0,  0,  0),
    };
    AABB a = computeMeshAABB(verts);
    REQUIRE(a.valid);
    REQUIRE(a.min == glm::vec3(-1.0f));
    REQUIRE(a.max == glm::vec3( 1.0f));
    REQUIRE(a.size()    == glm::vec3(2.0f));
    REQUIRE(a.center()  == glm::vec3(0.0f));
    REQUIRE(a.extents() == glm::vec3(1.0f));
    REQUIRE_THAT(a.longestAxis(), WithinAbs(2.0f, 1e-6f));
    // Radius = length(extents) = sqrt(3) for a unit cube.
    REQUIRE_THAT(a.radius(), WithinAbs(1.7320508f, 1e-4f));
}

TEST_CASE("computeMeshAABB: asymmetric box, longestAxis picks tallest dim",
          "[mesh-import][aabb]") {
    // A tall thin box:  x in [-0.5, +0.5], y in [0, 1.7], z in [-0.25, +0.25]
    // (roughly human-sized, matching the CC5 character framing use case).
    std::vector<MeshVertex> verts{
        vtx(-0.5f, 0.0f, -0.25f),
        vtx(+0.5f, 0.0f, -0.25f),
        vtx(-0.5f, 1.7f, -0.25f),
        vtx(+0.5f, 1.7f, -0.25f),
        vtx(-0.5f, 0.0f, +0.25f),
        vtx(+0.5f, 0.0f, +0.25f),
        vtx(-0.5f, 1.7f, +0.25f),
        vtx(+0.5f, 1.7f, +0.25f),
    };
    AABB a = computeMeshAABB(verts);
    REQUIRE(a.valid);
    REQUIRE_THAT(a.size().x, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(a.size().y, WithinAbs(1.7f, 1e-6f));
    REQUIRE_THAT(a.size().z, WithinAbs(0.5f, 1e-6f));
    // Longest axis should pick Y (1.7).
    REQUIRE_THAT(a.longestAxis(), WithinAbs(1.7f, 1e-6f));
    // Center should be mid-body: y=0.85.
    REQUIRE_THAT(a.center().y, WithinAbs(0.85f, 1e-6f));
    // x/z center should be 0.
    REQUIRE_THAT(a.center().x, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(a.center().z, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("computeMeshAABB: negative-coordinate meshes still compute correctly",
          "[mesh-import][aabb]") {
    // Entire mesh sits in octant x<0, y<0, z<0 — regression guard for
    // the "min initialized to vertex[0]" branch.
    std::vector<MeshVertex> verts{
        vtx(-5.0f, -4.0f, -3.0f),
        vtx(-2.0f, -1.0f, -0.5f),
        vtx(-10.0f, -2.0f, -1.5f),
    };
    AABB a = computeMeshAABB(verts);
    REQUIRE(a.valid);
    REQUIRE(a.min == glm::vec3(-10.0f, -4.0f, -3.0f));
    REQUIRE(a.max == glm::vec3(-2.0f, -1.0f, -0.5f));
    REQUIRE(a.size() == glm::vec3(8.0f, 3.0f, 2.5f));
    REQUIRE_THAT(a.longestAxis(), WithinAbs(8.0f, 1e-6f));
}
