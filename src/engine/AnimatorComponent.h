// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ECS component for skeletal animation.
// Attach to any entity with a skinned VkMesh to drive animation.
// Games update AnimatorComponent each frame, then the renderer
// uploads bone data via AnimationSystem and draws with boneOffset.
//
// Three playback modes:
//   1. Legacy: direct clip index + timeCursor (tick())
//   2. State machine: AnimationStateMachine drives blend layers
//   3. Blend tree: AnimBodyLayer[] with IBlendNode trees + joint masks
//      Use AnimationSystem::blendBodyLayers() with body layers owned by game code.
//
// IK + root motion:
//   After blending, optionally call extractRootMotion() then computeSkinningMatrices()
//   then applyIK(). IK slot arrays and root motion state live in this component.

#include "AnimationTypes.h"
#include "AnimationSystem.h"
#include <vector>
#include <cstdint>
#include <memory>

namespace sv {

class AnimationStateMachine; // forward — defined in AnimationStateMachine.h
class IAnimationController;  // forward — defined in IAnimationController.h

struct AnimatorComponent {
    // ── Skeleton & instance ────────────────────────────────────────
    SkeletonHandle    skeleton;    // shared skeleton reference
    AnimationInstance instance;    // per-character sampling state

    // ── Clip library ───────────────────────────────────────────────
    std::vector<AnimationClip> clips;  // available animation clips

    // ── Playback state (legacy mode) ──────────────────────────────
    int     activeClipIndex = -1;  // index into clips (-1 = rest pose / T-pose)
    float   playbackSpeed   = 1.0f;
    float   timeCursor      = 0.0f; // seconds (wraps automatically)

    // ── Blend weights (for multi-clip blending) ────────────────────
    // One weight per clip. If empty, activeClipIndex is used at weight 1.0.
    std::vector<float> blendWeights;

    // ── GPU state (set by renderer each frame) ─────────────────────
    uint32_t boneOffset = 0;  // SSBO offset returned by uploadBones()

    // ── State machine ─────────────────────────────────────
    std::unique_ptr<AnimationStateMachine> stateMachine;  // owned, nullable
    IAnimationController*                  controller = nullptr;  // game-provided, non-owning

    // ── IK targets ────────────────────────────────────────
    // Game code populates these each frame, then passes to AnimationSystem::applyIK().
    static constexpr int MAX_IK_TWOBONES = 4;  // feet + hands
    static constexpr int MAX_IK_AIMS     = 2;  // head + spine
    TwoBoneIKSlot twoBoneIK[MAX_IK_TWOBONES];
    int           twoBoneIKCount = 0;
    AimIKSlot     aimIK[MAX_IK_AIMS];
    int           aimIKCount = 0;

    // ── Root motion ──────────────────────────────────────
    bool      rootMotionEnabled = false;
    glm::vec3 prevRootPos{0.0f};   // tracks previous frame root translation
    glm::quat prevRootRot{1.0f, 0.0f, 0.0f, 0.0f};  // tracks previous frame root rotation

    // ── Helpers ────────────────────────────────────────────────────

    bool hasAnimation() const { return !clips.empty(); }
    bool hasStateMachine() const { return stateMachine != nullptr; }

    // Advance time cursor by dt, wrapping if a clip is active.
    // Legacy path — used when no state machine is present.
    void tick(float dt) {
        timeCursor += dt * playbackSpeed;
        if (activeClipIndex >= 0 && activeClipIndex < (int)clips.size()) {
            float dur = clips[activeClipIndex].duration;
            if (dur > 0.0f && timeCursor >= dur)
                timeCursor -= dur * std::floor(timeCursor / dur);
        }
    }
};

} // namespace sv
