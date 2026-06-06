// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── S-T1: SceneNode / hierarchy unit tests ──────────────────────────
// Tests for SceneLoader's public-facing TRS composition math.
//
// SceneLoader::loadFromFile() requires a VkCtx, so we can't exercise the full
// JSON → mesh pipeline from a unit test. But SceneNode::localMatrix() is the
// TRS composition primitive used by computeWorldTransforms(), and the
// parent → world composition is straightforward matrix multiplication.
//
// These tests verify the actual math that SceneLoader relies on.

#include "SceneLoader.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

using sv::SceneNode;
using sv::SceneMarker;
using Catch::Matchers::WithinAbs;

namespace {

bool vecNear(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return std::abs(a.x - b.x) < eps &&
           std::abs(a.y - b.y) < eps &&
           std::abs(a.z - b.z) < eps;
}

glm::vec3 translationOf(const glm::mat4& m) {
    return glm::vec3(m[3]);
}

} // anonymous

// ── localMatrix TRS composition ──────────────────────────────────────

TEST_CASE("SceneNode: default localMatrix is identity", "[scene-node]") {
    SceneNode node;
    glm::mat4 m = node.localMatrix();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            REQUIRE_THAT(m[c][r], WithinAbs((c == r) ? 1.0f : 0.0f, 1e-6f));
}

TEST_CASE("SceneNode: translation-only TRS", "[scene-node]") {
    SceneNode node;
    node.position = glm::vec3(3, 4, 5);
    glm::mat4 m = node.localMatrix();
    REQUIRE(vecNear(translationOf(m), glm::vec3(3, 4, 5)));
    // Rotation/scale portion should still be identity
    REQUIRE_THAT(m[0][0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(m[1][1], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(m[2][2], WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("SceneNode: scale-only TRS", "[scene-node]") {
    SceneNode node;
    node.localScale = glm::vec3(2, 3, 4);
    glm::mat4 m = node.localMatrix();

    // Transforming unit X should give (2, 0, 0)
    glm::vec3 xTransformed = glm::vec3(m * glm::vec4(1, 0, 0, 1));
    REQUIRE(vecNear(xTransformed, glm::vec3(2, 0, 0)));

    glm::vec3 yTransformed = glm::vec3(m * glm::vec4(0, 1, 0, 1));
    REQUIRE(vecNear(yTransformed, glm::vec3(0, 3, 0)));

    glm::vec3 zTransformed = glm::vec3(m * glm::vec4(0, 0, 1, 1));
    REQUIRE(vecNear(zTransformed, glm::vec3(0, 0, 4)));
}

TEST_CASE("SceneNode: 90 deg rotation around Y", "[scene-node]") {
    SceneNode node;
    // 90 deg rotation around Y: X axis should map to -Z
    node.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    glm::mat4 m = node.localMatrix();

    glm::vec3 x = glm::vec3(m * glm::vec4(1, 0, 0, 0));
    REQUIRE(vecNear(x, glm::vec3(0, 0, -1)));
}

TEST_CASE("SceneNode: combined TRS order is T * R * S", "[scene-node]") {
    // Build a node with non-trivial T, R, S and verify the localMatrix
    // equals glm::translate * glm::rotate * glm::scale in that order.
    SceneNode node;
    node.position   = glm::vec3(1, 2, 3);
    node.rotation   = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 0, 1));
    node.localScale = glm::vec3(2, 2, 2);

    glm::mat4 expected = glm::translate(glm::mat4(1.0f), node.position)
                        * glm::mat4_cast(node.rotation)
                        * glm::scale(glm::mat4(1.0f), node.localScale);

    glm::mat4 actual = node.localMatrix();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            REQUIRE_THAT(actual[c][r], WithinAbs(expected[c][r], 1e-5f));
}

TEST_CASE("SceneNode: negative scale inverts axis", "[scene-node]") {
    SceneNode node;
    node.localScale = glm::vec3(-1, 1, 1);
    glm::mat4 m = node.localMatrix();

    glm::vec3 x = glm::vec3(m * glm::vec4(1, 0, 0, 1));
    REQUIRE(vecNear(x, glm::vec3(-1, 0, 0)));
}

// ── Parent/child world transform composition ────────────────────────
// Replicates SceneLoader::computeWorldTransformRecursive math.

TEST_CASE("SceneNode: child world pos = parent world * child local", "[scene-node]") {
    SceneNode parent;
    parent.position = glm::vec3(10, 0, 0);
    parent.worldTransform = parent.localMatrix();

    SceneNode child;
    child.parent = 0;
    child.position = glm::vec3(0, 5, 0);
    child.worldTransform = parent.worldTransform * child.localMatrix();

    // Child world position should be (10, 5, 0)
    glm::vec3 childWorld = translationOf(child.worldTransform);
    REQUIRE(vecNear(childWorld, glm::vec3(10, 5, 0)));
}

TEST_CASE("SceneNode: parent rotation propagates to child translation", "[scene-node]") {
    // Parent at origin, rotated 90 deg around Y.
    SceneNode parent;
    parent.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    parent.worldTransform = parent.localMatrix();

    // Child offset (1, 0, 0) in local space should end up at (0, 0, -1) in world.
    SceneNode child;
    child.position = glm::vec3(1, 0, 0);
    child.worldTransform = parent.worldTransform * child.localMatrix();

    glm::vec3 childWorld = translationOf(child.worldTransform);
    REQUIRE(vecNear(childWorld, glm::vec3(0, 0, -1)));
}

TEST_CASE("SceneNode: grandchild accumulates translation", "[scene-node]") {
    SceneNode root;
    root.position = glm::vec3(1, 0, 0);
    root.worldTransform = root.localMatrix();

    SceneNode child;
    child.parent = 0;
    child.position = glm::vec3(0, 2, 0);
    child.worldTransform = root.worldTransform * child.localMatrix();

    SceneNode grandchild;
    grandchild.parent = 1;
    grandchild.position = glm::vec3(0, 0, 3);
    grandchild.worldTransform = child.worldTransform * grandchild.localMatrix();

    glm::vec3 grandchildWorld = translationOf(grandchild.worldTransform);
    REQUIRE(vecNear(grandchildWorld, glm::vec3(1, 2, 3)));
}

// ── SceneMarker ──────────────────────────────────────────────────────

TEST_CASE("SceneMarker: default matrix is identity", "[scene-node][marker]") {
    SceneMarker m;
    glm::mat4 mat = m.matrix();
    REQUIRE_THAT(mat[0][0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(mat[1][1], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(mat[2][2], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(mat[3][0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(mat[3][1], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(mat[3][2], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("SceneMarker: position populates translation column", "[scene-node][marker]") {
    SceneMarker m;
    m.position = glm::vec3(7, 8, 9);
    glm::mat4 mat = m.matrix();
    REQUIRE(vecNear(translationOf(mat), glm::vec3(7, 8, 9)));
}
