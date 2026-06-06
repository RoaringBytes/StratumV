// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// Animation types for the ozz-animation integration.
// SkeletonHandle wraps an ozz runtime Skeleton built from glTF skin data.
// AnimationClip wraps an ozz runtime Animation built from glTF animation tracks.

#include <ozz/animation/runtime/skeleton.h>
#include <ozz/animation/runtime/animation.h>
#include <memory>
#include <string>
#include <vector>

namespace sv {

struct SkeletonData; // forward — defined in vk/VkMesh.h

// Runtime skeleton built from SkeletonData (glTF skin) via ozz offline API.
struct SkeletonHandle {
    std::shared_ptr<ozz::animation::Skeleton> skeleton;

    // Joint order mapping: ozzToDataIndex[ozz_joint] = SkeletonData joint index.
    // Needed because ozz stores joints in depth-first order, which may differ
    // from the glTF skin.joints order used by vertex JOINTS_0 attributes.
    std::vector<int> ozzToDataIndex;

    int jointCount() const {
        return skeleton ? skeleton->num_joints() : 0;
    }
    int soaCount() const {
        return skeleton ? skeleton->num_soa_joints() : 0;
    }
    int dataJointCount() const {
        return (int)ozzToDataIndex.size();
    }
    explicit operator bool() const { return skeleton != nullptr; }
};

// Runtime animation clip built from glTF animation tracks.
struct AnimationClip {
    std::string name;
    float       duration = 0.0f;
    std::shared_ptr<ozz::animation::Animation> animation;

    explicit operator bool() const { return animation != nullptr; }
};

// Build an ozz runtime Skeleton from parsed glTF skin data.
// Uses rest pose TRS from SkeletonJoint to define the bind pose.
SkeletonHandle buildSkeleton(const SkeletonData& data);

// Load all animations from a glTF file, converting to ozz runtime format.
// Joint mapping uses the skeleton's joint names to correlate with glTF node names.
// Returns one AnimationClip per glTF animation found in the file.
std::vector<AnimationClip> loadGltfAnimations(const std::string& path,
                                               const SkeletonHandle& skeleton);

// Load all animations from an FBX file via ufbx, converting to ozz runtime format.
// Each FBX anim_stack becomes one AnimationClip.  Bone transforms are sampled at
// 30 fps and converted to ozz RawAnimation tracks using ufbx_evaluate_transform().
std::vector<AnimationClip> loadFbxAnimations(const std::string& path,
                                              const SkeletonHandle& skeleton);

} // namespace sv
