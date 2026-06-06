// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// Blend tree nodes for animation blending.
// Provides a node-based abstraction for parametric blending (1D blend space),
// partial body blending (per-joint weight masks), and additive layer support.
//
// Nodes produce local-space poses. Body layers combine them with per-joint masks
// via AnimationSystem::blendBodyLayers().

#include "AnimationTypes.h"

#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/maths/simd_math.h>

#include <vector>
#include <memory>
#include <cstdint>

namespace sv {

// ── IBlendNode ──────────────────────────────────────────────────────
// Abstract blend tree node. Each node produces a local-space pose
// (SoaTransform array, one element per 4-joint SoA group).

class IBlendNode {
public:
    virtual ~IBlendNode() = default;

    // Allocate internal buffers for the given skeleton.
    virtual void init(const SkeletonHandle& skeleton) = 0;

    // Advance internal time by dt and write local-space transforms to output.
    // output must have at least skeleton.soaCount() elements.
    virtual void evaluate(float dt, ozz::span<ozz::math::SoaTransform> output) = 0;
};

// ── ClipNode ────────────────────────────────────────────────────────
// Leaf: plays a single animation clip with looping and speed control.

class ClipNode : public IBlendNode {
public:
    explicit ClipNode(const AnimationClip* clip, bool loop = true, float speed = 1.0f);

    void init(const SkeletonHandle& skeleton) override;
    void evaluate(float dt, ozz::span<ozz::math::SoaTransform> output) override;

    void  setTime(float t)  { m_time = t; }
    float time()      const { return m_time; }
    float duration()  const { return m_clip ? m_clip->duration : 0.0f; }
    void  setSpeed(float s) { m_speed = s; }
    void  setLoop(bool l)   { m_loop = l; }

private:
    const AnimationClip* m_clip;
    bool  m_loop;
    float m_speed;
    float m_time = 0.0f;
    int   m_numJoints = 0;
    std::unique_ptr<ozz::animation::SamplingJob::Context> m_context;
};

// ── RestPoseNode ────────────────────────────────────────────────────
// Constant output: skeleton rest pose (T-pose / bind pose).
// Useful as a blend space endpoint or placeholder layer.

class RestPoseNode : public IBlendNode {
public:
    void init(const SkeletonHandle& skeleton) override;
    void evaluate(float dt, ozz::span<ozz::math::SoaTransform> output) override;

private:
    const ozz::animation::Skeleton* m_skeleton = nullptr;
};

// ── BlendSpace1D ────────────────────────────────────────────────────
// 1D parametric blend between N clips sorted by threshold.
// Blends the two clips adjacent to the current parameter value.
// Example: idle(0) -> walk(1) -> run(3) driven by velocity magnitude.

struct BlendEntry1D {
    const AnimationClip* clip = nullptr; // null = rest pose at this threshold
    float threshold = 0.0f;
};

class BlendSpace1D : public IBlendNode {
public:
    explicit BlendSpace1D(std::vector<BlendEntry1D> entries);

    void init(const SkeletonHandle& skeleton) override;
    void evaluate(float dt, ozz::span<ozz::math::SoaTransform> output) override;

    void  setParameter(float p) { m_parameter = p; }
    float parameter()     const { return m_parameter; }
    int   entryCount()    const { return (int)m_entries.size(); }
    float threshold(int i) const;

private:
    std::vector<BlendEntry1D> m_entries; // sorted by threshold
    float m_parameter = 0.0f;

    struct EntryState {
        std::unique_ptr<ozz::animation::SamplingJob::Context> context;
        ozz::vector<ozz::math::SoaTransform> locals;
        float time = 0.0f;
    };
    std::vector<EntryState> m_states;
    const ozz::animation::Skeleton* m_skeleton = nullptr;
    int m_numSoa    = 0;
    int m_numJoints = 0;
};

// ── AnimBodyLayer ───────────────────────────────────────────────────
// One layer in a multi-layer body blend. Each layer has a blend tree,
// an overall weight, and an optional per-joint mask for partial blending.

struct AnimBodyLayer {
    IBlendNode* node     = nullptr;  // blend tree root (non-owning)
    float       weight   = 1.0f;     // overall layer weight [0,1]
    bool        additive = false;    // if true, applied as ozz additive layer

    // Optional per-joint weight mask (null = all joints at full weight).
    // Array of SimdFloat4, size = skeleton.num_soa_joints().
    // Each SimdFloat4 contains weights for 4 joints in the SoA group.
    const ozz::math::SimdFloat4* jointWeights = nullptr;
};

// ── Joint mask builder ──────────────────────────────────────────────
// Build a per-joint weight mask for partial body blending.
// All joints that are descendants of (or equal to) rootJointName get insideWeight,
// all others get outsideWeight.
// Returns a SoA-packed vector (size = num_soa_joints).
ozz::vector<ozz::math::SimdFloat4> buildJointMask(
    const SkeletonHandle& skeleton,
    const char*           rootJointName,
    float                 insideWeight  = 1.0f,
    float                 outsideWeight = 0.0f);

} // namespace sv
