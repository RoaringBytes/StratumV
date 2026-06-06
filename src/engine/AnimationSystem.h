// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// Animation runtime system (extended for IK + root motion).
// Per-frame pipeline: SamplingJob -> BlendingJob -> LocalToModelJob -> IK -> skinning matrices -> SSBO upload.
// SSBO layout: std430 readonly buffer BonePalette { mat4 bones[]; } at set 1 binding 0.

#include "AnimationTypes.h"
#include "vk/VkBuffer.h"

#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/maths/simd_math.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <volk.h>
#include <vk_mem_alloc.h>

#include <vector>
#include <cstdint>

namespace sv {

struct SkeletonData; // forward

// ── IK target slots ───────────────────────────────────────
// Game code fills these and passes to AnimationSystem::applyIK().
// Joint indices are ozz joint order (from SkeletonHandle::skeleton->joint_names()).

struct TwoBoneIKSlot {
    int startJoint = -1;  // ozz joint index (e.g., upper leg / upper arm)
    int midJoint   = -1;  // ozz joint index (e.g., knee / elbow)
    int endJoint   = -1;  // ozz joint index (e.g., ankle / wrist)
    glm::vec3 target{0.0f};                   // model-space target position
    glm::vec3 poleVector{0.0f, 0.0f, 1.0f};   // pole vector in model-space (knee/elbow direction)
    glm::vec3 midAxis{0.0f, 0.0f, 1.0f};      // mid joint rotation axis in local-space
    float weight = 0.0f;   // 0 = disabled, 1 = full IK
    float soften = 1.0f;   // distance ratio where IK starts to soften
};

struct AimIKSlot {
    int joint = -1;                            // ozz joint index (e.g., head / spine)
    glm::vec3 target{0.0f};                    // model-space aim target
    glm::vec3 forward{1.0f, 0.0f, 0.0f};      // joint forward axis (local-space)
    glm::vec3 up{0.0f, 1.0f, 0.0f};           // joint up axis (local-space)
    glm::vec3 poleVector{0.0f, 1.0f, 0.0f};   // pole vector (model-space)
    float weight = 0.0f;
};

// ── Root motion delta ─────────────────────────────────────
struct RootMotionDelta {
    glm::vec3 deltaPosition{0.0f};
    glm::quat deltaRotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Per-character animation state.
struct AnimationInstance {
    SkeletonHandle skeleton;

    // ozz sampling context (reused across frames for cache coherency).
    // unique_ptr because Context is non-copyable and non-movable.
    std::unique_ptr<ozz::animation::SamplingJob::Context> samplingContext;

    // Intermediate buffers (SoA layout — ceil(joints/4) elements)
    ozz::vector<ozz::math::SoaTransform> locals;

    // Blending output (same size as locals)
    ozz::vector<ozz::math::SoaTransform> blended;

    // Model-space matrices (one per joint)
    ozz::vector<ozz::math::Float4x4> models;

    // Final skinning matrices: models[i] * inverseBindMatrix[i] (glm for GPU upload)
    std::vector<glm::mat4> skinningMatrices;
};

// Blend layer for multi-clip blending.
struct BlendLayer {
    const AnimationClip* clip   = nullptr;
    float                weight = 1.0f;
    float                time   = 0.0f; // seconds

    // Optional per-joint weight mask for partial blending.
    // Null = all joints at full weight.
    // Array of SimdFloat4, size = skeleton.num_soa_joints().
    const ozz::math::SimdFloat4* jointWeights = nullptr;
};

struct AnimBodyLayer; // forward — defined in BlendTree.h

// Manages the animation pipeline and SSBO bone palette.
class AnimationSystem {
public:
    // maxBones: total bone capacity across all characters this frame.
    void init(VkDevice device, VmaAllocator allocator, uint32_t maxBones = 4096);
    void destroy();

    // ── Instance lifecycle ──────────────────────────────────────────
    AnimationInstance createInstance(const SkeletonHandle& skeleton);

    // ── Per-frame pipeline ──────────────────────────────────────────

    // Sample a single clip into instance.locals at the given time (seconds).
    void sample(AnimationInstance& inst, const AnimationClip& clip, float timeSeconds);

    // Blend multiple layers into instance.blended.
    // Falls back to identity if layers is empty.
    void blend(AnimationInstance& inst, const BlendLayer* layers, int layerCount);

    // LocalToModel + skinning matrix computation.
    // Uses inst.blended (or inst.locals if blended is stale) as input.
    // Writes inst.models and inst.skinningMatrices.
    void computeSkinningMatrices(AnimationInstance& inst, const SkeletonData& bindPose);

    // ── SSBO bone palette ───────────────────────────────────────────

    // Call at frame start — resets the write cursor to 0.
    void resetBonePalette();

    // Upload an instance's skinning matrices to the SSBO.
    // Returns the bone offset (in matrix count) for use as push constant.
    uint32_t uploadBones(const AnimationInstance& inst);

    // Flush staged bone data to the GPU buffer.
    // Call after all uploadBones() calls, before rendering.
    void flushBonePalette(VkDevice device, VmaAllocator allocator);

    // Blend body layers with per-joint weight masks.
    // Each layer's blend tree node is evaluated (advanced by dt), then blended
    // using ozz BlendingJob with per-layer joint_weights. Additive layers are
    // routed to BlendingJob::additive_layers. Writes to inst.blended.
    void blendBodyLayers(AnimationInstance& inst, AnimBodyLayer* layers, int layerCount, float dt);

    // ── IK post-processing ────────────────────────────────

    // Apply IK corrections after L2M. Caller must call computeSkinningMatrices()
    // first so inst.models is populated. This method:
    //   1. Runs IKTwoBoneJob / IKAimJob using inst.models
    //   2. Applies correction quaternions to inst.blended (local transforms)
    //   3. Re-runs L2M + skinning to produce corrected inst.models and inst.skinningMatrices
    void applyIK(AnimationInstance& inst, const SkeletonData& bindPose,
                 const TwoBoneIKSlot* twoBone, int twoBoneCount,
                 const AimIKSlot* aim, int aimCount);

    // ── Root motion extraction ─────────────────────────────

    // Extract root motion delta from inst.blended (local transforms).
    // Reads root joint (ozz index 0) translation, computes delta from prevRootPos,
    // zeroes root XZ translation in inst.blended. Call BEFORE computeSkinningMatrices().
    // prevRootPos is updated in-place to the current root position for next frame.
    RootMotionDelta extractRootMotion(AnimationInstance& inst,
                                       glm::vec3& prevRootPos, glm::quat& prevRootRot);

    // ── Accessors ───────────────────────────────────────────────────
    VkBuffer              bonePaletteBuffer()    const { return m_bonePaletteSSBO.buffer; }
    VkDescriptorSetLayout bonePaletteLayout()    const { return m_bonePaletteLayout; }
    VkDescriptorSet       bonePaletteDescSet()   const { return m_bonePaletteDescSet; }
    uint32_t              maxBones()             const { return m_maxBones; }
    uint32_t              currentBoneOffset()    const { return m_boneWriteCursor; }

private:
    VkDevice              m_device    = VK_NULL_HANDLE;
    VmaAllocator          m_allocator = VK_NULL_HANDLE;

    // SSBO bone palette
    VkBuf                 m_bonePaletteSSBO{};
    VkDescriptorSetLayout m_bonePaletteLayout  = VK_NULL_HANDLE;
    VkDescriptorPool      m_bonePalettePool    = VK_NULL_HANDLE;
    VkDescriptorSet       m_bonePaletteDescSet = VK_NULL_HANDLE;
    uint32_t              m_maxBones       = 0;
    uint32_t              m_boneWriteCursor = 0;

    // CPU staging buffer (flushed to GPU each frame)
    std::vector<glm::mat4> m_cpuBoneStaging;
};

} // namespace sv
