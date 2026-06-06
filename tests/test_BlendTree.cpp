// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── S-T1: BlendTree unit tests ───────────────────────────────────────
// Tests for sv::BlendSpace1D (sorting + accessors) and sv::buildJointMask
// (descendant propagation over an ozz skeleton).
//
// Strategy: we build a tiny ozz::Skeleton from a RawSkeleton with 4 joints
//  (root -> upper, root -> hand; upper -> lower) and verify:
//   - the joint mask for "root" covers every joint
//   - the joint mask for "upper" covers upper + lower only
//   - the joint mask for an unknown joint is all-zero
//
// BlendSpace1D is tested via its public constructor + threshold accessor
// without init() (which requires a skeleton).

#include "BlendTree.h"
#include "AnimationTypes.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/memory/allocator.h>

#include <cstring>

using sv::BlendSpace1D;
using sv::BlendEntry1D;
using sv::AnimationClip;
using sv::SkeletonHandle;
using Catch::Matchers::WithinAbs;

namespace {

// Build a minimal 4-joint test skeleton:
//   root
//   ├── upper
//   │   └── lower
//   └── hand
SkeletonHandle buildTestSkeleton() {
    using namespace ozz::animation::offline;

    RawSkeleton raw;
    raw.roots.resize(1);

    auto& root = raw.roots[0];
    root.name = "root";
    root.children.resize(2);

    auto& upper = root.children[0];
    upper.name = "upper";
    upper.children.resize(1);

    auto& lower = upper.children[0];
    lower.name = "lower";

    auto& hand = root.children[1];
    hand.name = "hand";

    SkeletonBuilder builder;
    auto built = builder(raw);
    REQUIRE(built);

    // ozz allocates Skeleton via a custom heap allocator (malloc + alignment
    // header). A std::shared_ptr with the default deleter (delete) would
    // corrupt the heap on destruction — we must route destruction back
    // through ozz::Delete which calls the matching Deallocate().
    SkeletonHandle handle;
    handle.skeleton = std::shared_ptr<ozz::animation::Skeleton>(
        built.release(),
        [](ozz::animation::Skeleton* s) { ozz::Delete(s); });
    return handle;
}

// Extract a per-joint weight from a SoA mask by joint index.
float maskWeight(const ozz::vector<ozz::math::SimdFloat4>& mask, int jointIdx) {
    int soaIdx   = jointIdx / 4;
    int laneIdx  = jointIdx % 4;
    alignas(16) float lanes[4];
    ozz::math::StorePtr(mask[soaIdx], lanes);
    return lanes[laneIdx];
}

int findJoint(const SkeletonHandle& handle, const char* name) {
    auto names = handle.skeleton->joint_names();
    for (int i = 0; i < handle.skeleton->num_joints(); ++i) {
        if (strcmp(names[i], name) == 0)
            return i;
    }
    return -1;
}

} // anonymous

// ── BlendSpace1D ──────────────────────────────────────────────────────

TEST_CASE("BlendSpace1D: constructor sorts entries by threshold", "[blendtree]") {
    AnimationClip walk; walk.name = "walk"; walk.duration = 1.0f;
    AnimationClip run;  run.name  = "run";  run.duration  = 0.8f;
    AnimationClip idle; idle.name = "idle"; idle.duration = 2.0f;

    // Intentionally unsorted
    std::vector<BlendEntry1D> entries = {
        { &run,  3.0f },
        { &idle, 0.0f },
        { &walk, 1.0f },
    };
    BlendSpace1D bs(std::move(entries));

    REQUIRE(bs.entryCount() == 3);
    REQUIRE_THAT(bs.threshold(0), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(bs.threshold(1), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(bs.threshold(2), WithinAbs(3.0f, 1e-6f));
}

TEST_CASE("BlendSpace1D: empty entry list is valid", "[blendtree]") {
    BlendSpace1D bs(std::vector<BlendEntry1D>{});
    REQUIRE(bs.entryCount() == 0);
}

TEST_CASE("BlendSpace1D: single entry", "[blendtree]") {
    AnimationClip clip; clip.duration = 1.0f;
    BlendSpace1D bs(std::vector<BlendEntry1D>{ { &clip, 0.0f } });
    REQUIRE(bs.entryCount() == 1);
    REQUIRE_THAT(bs.threshold(0), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("BlendSpace1D: threshold out-of-range returns 0", "[blendtree]") {
    BlendSpace1D bs(std::vector<BlendEntry1D>{});
    REQUIRE_THAT(bs.threshold(-1), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(bs.threshold(99), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("BlendSpace1D: setParameter is stored", "[blendtree]") {
    BlendSpace1D bs(std::vector<BlendEntry1D>{});
    bs.setParameter(2.5f);
    REQUIRE_THAT(bs.parameter(), WithinAbs(2.5f, 1e-6f));
}

// ── buildJointMask ────────────────────────────────────────────────────

TEST_CASE("buildJointMask: root mask covers every joint", "[blendtree][mask]") {
    auto handle = buildTestSkeleton();
    const int numJoints = handle.jointCount();
    REQUIRE(numJoints == 4);

    auto mask = sv::buildJointMask(handle, "root", 1.0f, 0.0f);
    for (int i = 0; i < numJoints; ++i) {
        REQUIRE_THAT(maskWeight(mask, i), WithinAbs(1.0f, 1e-4f));
    }
}

TEST_CASE("buildJointMask: subtree mask covers descendants only", "[blendtree][mask]") {
    auto handle = buildTestSkeleton();

    auto mask = sv::buildJointMask(handle, "upper", 1.0f, 0.0f);

    int rootIdx  = findJoint(handle, "root");
    int upperIdx = findJoint(handle, "upper");
    int lowerIdx = findJoint(handle, "lower");
    int handIdx  = findJoint(handle, "hand");

    REQUIRE(rootIdx  >= 0);
    REQUIRE(upperIdx >= 0);
    REQUIRE(lowerIdx >= 0);
    REQUIRE(handIdx  >= 0);

    // upper and lower get full weight; root and hand get zero
    REQUIRE_THAT(maskWeight(mask, upperIdx), WithinAbs(1.0f, 1e-4f));
    REQUIRE_THAT(maskWeight(mask, lowerIdx), WithinAbs(1.0f, 1e-4f));
    REQUIRE_THAT(maskWeight(mask, rootIdx),  WithinAbs(0.0f, 1e-4f));
    REQUIRE_THAT(maskWeight(mask, handIdx),  WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("buildJointMask: leaf joint mask only includes the leaf", "[blendtree][mask]") {
    auto handle = buildTestSkeleton();

    auto mask = sv::buildJointMask(handle, "hand", 1.0f, 0.0f);

    int handIdx = findJoint(handle, "hand");
    REQUIRE(handIdx >= 0);

    // Only the hand joint gets full weight
    for (int i = 0; i < handle.jointCount(); ++i) {
        float expected = (i == handIdx) ? 1.0f : 0.0f;
        REQUIRE_THAT(maskWeight(mask, i), WithinAbs(expected, 1e-4f));
    }
}

TEST_CASE("buildJointMask: unknown joint returns outsideWeight everywhere", "[blendtree][mask]") {
    auto handle = buildTestSkeleton();

    auto mask = sv::buildJointMask(handle, "nonexistent", 1.0f, 0.25f);
    for (int i = 0; i < handle.jointCount(); ++i) {
        REQUIRE_THAT(maskWeight(mask, i), WithinAbs(0.25f, 1e-4f));
    }
}

TEST_CASE("buildJointMask: custom inside/outside weights", "[blendtree][mask]") {
    auto handle = buildTestSkeleton();

    auto mask = sv::buildJointMask(handle, "upper", 0.75f, 0.1f);

    int upperIdx = findJoint(handle, "upper");
    int lowerIdx = findJoint(handle, "lower");
    int rootIdx  = findJoint(handle, "root");

    REQUIRE_THAT(maskWeight(mask, upperIdx), WithinAbs(0.75f, 1e-4f));
    REQUIRE_THAT(maskWeight(mask, lowerIdx), WithinAbs(0.75f, 1e-4f));
    REQUIRE_THAT(maskWeight(mask, rootIdx),  WithinAbs(0.1f,  1e-4f));
}
