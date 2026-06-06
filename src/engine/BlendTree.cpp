// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#ifdef _WIN32
#define NOMINMAX
#endif

#include "BlendTree.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace sv {

// ── ClipNode ────────────────────────────────────────────────────────

ClipNode::ClipNode(const AnimationClip* clip, bool loop, float speed)
    : m_clip(clip), m_loop(loop), m_speed(speed) {}

void ClipNode::init(const SkeletonHandle& skeleton) {
    m_numJoints = skeleton.jointCount();
    m_context = std::make_unique<ozz::animation::SamplingJob::Context>(m_numJoints);
}

void ClipNode::evaluate(float dt, ozz::span<ozz::math::SoaTransform> output) {
    if (!m_clip || !m_clip->animation || !m_context) return;

    // Advance time
    m_time += dt * m_speed;
    float dur = m_clip->duration;
    if (m_loop && dur > 0.0f) {
        m_time = m_time - dur * std::floor(m_time / dur);
    } else if (dur > 0.0f) {
        m_time = (std::min)(m_time, dur);
    }

    float ratio = (dur > 0.0f) ? (m_time / dur) : 0.0f;
    ratio = std::clamp(ratio, 0.0f, 1.0f);

    ozz::animation::SamplingJob job;
    job.animation = m_clip->animation.get();
    job.context   = m_context.get();
    job.ratio     = ratio;
    job.output    = output;

    if (!job.Run()) {
        printf("[ClipNode] SamplingJob failed\n");
    }
}

// ── RestPoseNode ────────────────────────────────────────────────────

void RestPoseNode::init(const SkeletonHandle& skeleton) {
    m_skeleton = skeleton.skeleton.get();
}

void RestPoseNode::evaluate(float /*dt*/, ozz::span<ozz::math::SoaTransform> output) {
    if (!m_skeleton) return;
    auto rest = m_skeleton->joint_rest_poses();
    std::copy(rest.begin(), rest.end(), output.begin());
}

// ── BlendSpace1D ────────────────────────────────────────────────────

BlendSpace1D::BlendSpace1D(std::vector<BlendEntry1D> entries)
    : m_entries(std::move(entries))
{
    std::sort(m_entries.begin(), m_entries.end(),
              [](const BlendEntry1D& a, const BlendEntry1D& b) {
                  return a.threshold < b.threshold;
              });
}

float BlendSpace1D::threshold(int i) const {
    if (i < 0 || i >= (int)m_entries.size()) return 0.0f;
    return m_entries[i].threshold;
}

void BlendSpace1D::init(const SkeletonHandle& skeleton) {
    m_skeleton  = skeleton.skeleton.get();
    m_numSoa    = skeleton.soaCount();
    m_numJoints = skeleton.jointCount();

    m_states.resize(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); i++) {
        if (m_entries[i].clip && m_entries[i].clip->animation)
            m_states[i].context = std::make_unique<ozz::animation::SamplingJob::Context>(m_numJoints);
        m_states[i].locals.resize(m_numSoa);
    }
}

void BlendSpace1D::evaluate(float dt, ozz::span<ozz::math::SoaTransform> output) {
    if (m_entries.empty() || !m_skeleton) return;

    // Clamp parameter to entry range
    float p = std::clamp(m_parameter, m_entries.front().threshold, m_entries.back().threshold);

    // Find the two adjacent entries that bracket the parameter
    int loIdx = 0, hiIdx = 0;
    for (int i = 0; i < (int)m_entries.size() - 1; i++) {
        if (p >= m_entries[i].threshold && p <= m_entries[i + 1].threshold) {
            loIdx = i;
            hiIdx = i + 1;
            break;
        }
        if (p > m_entries[i].threshold)
            loIdx = hiIdx = i;
    }
    if (p >= m_entries.back().threshold)
        loIdx = hiIdx = (int)m_entries.size() - 1;

    // Compute interpolation factor
    float alpha = 0.0f;
    if (loIdx != hiIdx) {
        float range = m_entries[hiIdx].threshold - m_entries[loIdx].threshold;
        if (range > 0.0f)
            alpha = (p - m_entries[loIdx].threshold) / range;
    }

    // Sample an entry: either from clip or rest pose
    auto sampleEntry = [&](int idx) {
        auto& state = m_states[idx];
        const auto& entry = m_entries[idx];

        if (!entry.clip || !entry.clip->animation) {
            auto rest = m_skeleton->joint_rest_poses();
            std::copy(rest.begin(), rest.end(), state.locals.begin());
            return;
        }

        state.time += dt;
        float dur = entry.clip->duration;
        if (dur > 0.0f)
            state.time = state.time - dur * std::floor(state.time / dur);

        float ratio = (dur > 0.0f) ? (state.time / dur) : 0.0f;
        ratio = std::clamp(ratio, 0.0f, 1.0f);

        ozz::animation::SamplingJob job;
        job.animation = entry.clip->animation.get();
        job.context   = state.context.get();
        job.ratio     = ratio;
        job.output    = ozz::make_span(state.locals);
        job.Run();
    };

    sampleEntry(loIdx);
    if (hiIdx != loIdx)
        sampleEntry(hiIdx);

    // Single entry or alpha at boundary — copy directly
    if (loIdx == hiIdx || alpha < 0.001f) {
        std::copy(m_states[loIdx].locals.begin(), m_states[loIdx].locals.end(), output.begin());
        return;
    }
    if (alpha > 0.999f) {
        std::copy(m_states[hiIdx].locals.begin(), m_states[hiIdx].locals.end(), output.begin());
        return;
    }

    // Blend between lo and hi
    ozz::animation::BlendingJob::Layer layers[2];
    layers[0].transform = ozz::make_span(m_states[loIdx].locals);
    layers[0].weight    = 1.0f - alpha;
    layers[1].transform = ozz::make_span(m_states[hiIdx].locals);
    layers[1].weight    = alpha;

    ozz::animation::BlendingJob blendJob;
    blendJob.threshold = ozz::animation::BlendingJob().threshold;
    blendJob.layers    = ozz::make_span(layers);
    blendJob.rest_pose = m_skeleton->joint_rest_poses();
    blendJob.output    = output;

    if (!blendJob.Run()) {
        printf("[BlendSpace1D] BlendingJob failed\n");
    }
}

// ── buildJointMask ──────────────────────────────────────────────────

ozz::vector<ozz::math::SimdFloat4> buildJointMask(
    const SkeletonHandle& skeleton,
    const char*           rootJointName,
    float                 insideWeight,
    float                 outsideWeight)
{
    int numJoints = skeleton.jointCount();
    int numSoa    = skeleton.soaCount();

    // Per-joint descendant flag
    std::vector<bool> isInside(numJoints, false);

    auto names   = skeleton.skeleton->joint_names();
    auto parents = skeleton.skeleton->joint_parents();

    int rootIdx = -1;
    for (int i = 0; i < numJoints; i++) {
        if (strcmp(names[i], rootJointName) == 0) {
            rootIdx = i;
            break;
        }
    }

    if (rootIdx >= 0) {
        isInside[rootIdx] = true;
        // ozz joints are depth-first — forward scan propagates to all descendants
        for (int i = rootIdx + 1; i < numJoints; i++) {
            int p = parents[i];
            if (p >= 0 && isInside[p])
                isInside[i] = true;
        }
    } else {
        printf("[BlendTree] buildJointMask: joint '%s' not found\n", rootJointName);
    }

    // Pack into SoA SimdFloat4 (4 joints per element)
    ozz::vector<ozz::math::SimdFloat4> mask(numSoa);
    for (int s = 0; s < numSoa; s++) {
        float w[4];
        for (int j = 0; j < 4; j++) {
            int idx = s * 4 + j;
            w[j] = (idx < numJoints && isInside[idx]) ? insideWeight : outsideWeight;
        }
        mask[s] = ozz::math::simd_float4::Load(w[0], w[1], w[2], w[3]);
    }

    return mask;
}

} // namespace sv
