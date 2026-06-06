// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// Animation state machine.
// Data-driven state graph with named states, trigger-based transitions,
// and crossfade blending. Produces BlendLayer arrays for AnimationSystem::blend().
//
// DLL-safe: all public API uses const char* and fixed-size char arrays.
// No std::function or std::string crosses the DLL boundary.

#include "AnimationTypes.h"
#include "AnimationSystem.h" // BlendLayer

#include <vector>
#include <cstdint>
#include <cstring>

namespace sv {

// ── Constants ────────────────────────────────────────────────────────
static constexpr int kMaxTriggerNameLen = 32;
static constexpr int kMaxActiveTriggers = 8;

// ── Transition mode ──────────────────────────────────────────────────
enum class TransitionMode : uint8_t {
    Immediate,   // crossfade starts as soon as trigger fires
    WaitForEnd   // crossfade starts when source clip reaches its end (non-looping only)
};

// ── State ────────────────────────────────────────────────────────────
struct AnimState {
    char                 name[kMaxTriggerNameLen] = {};
    const AnimationClip* clip                     = nullptr;
    bool                 loop                     = true;
    float                speed                    = 1.0f;
    float                timeCursor               = 0.0f; // internal, seconds
};

// ── Transition edge ──────────────────────────────────────────────────
struct AnimTransition {
    char            fromState[kMaxTriggerNameLen] = {};
    char            toState[kMaxTriggerNameLen]   = {};
    char            trigger[kMaxTriggerNameLen]   = {};  // empty = auto (WaitForEnd only)
    float           crossfadeDuration             = 0.2f;
    TransitionMode  mode                          = TransitionMode::Immediate;
};

// ── AnimationStateMachine ────────────────────────────────────────────
class AnimationStateMachine {
public:
    AnimationStateMachine()  = default;
    ~AnimationStateMachine() = default;

    // ── State graph construction ─────────────────────────────────
    void addState(const char* name, const AnimationClip* clip,
                  bool looping = true, float speed = 1.0f);

    void addTransition(const char* from, const char* to,
                       float crossfadeDuration, const char* trigger,
                       TransitionMode mode = TransitionMode::Immediate);

    void setInitialState(const char* name);

    // ── Per-frame update ─────────────────────────────────────────
    void update(float dt);

    // ── Trigger system (DLL-safe: const char* only) ──────────────
    void setTrigger(const char* name);
    void clearTrigger(const char* name);
    void clearAllTriggers();

    // ── Force transition (bypasses trigger check) ────────────────
    void triggerTransition(const char* targetState, float duration = -1.0f);

    // ── Query ────────────────────────────────────────────────────
    const char*      getCurrentStateName()     const;
    bool             isTransitioning()         const;
    float            getTransitionProgress()   const;
    float            getCurrentTime()          const;
    float            getCurrentDuration()      const;
    int              getStateCount()           const;
    const AnimState* getState(int index)       const;

    // ── BlendLayer output (for AnimationSystem::blend) ───────────
    // Returns layer count (1 = single state, 2 = crossfading).
    int getBlendLayers(BlendLayer* outLayers, int maxLayers) const;

private:
    std::vector<AnimState>      m_states;
    std::vector<AnimTransition> m_transitions;

    int   m_currentStateIndex = -1;
    int   m_targetStateIndex  = -1;    // -1 = not transitioning
    float m_crossfadeElapsed  = 0.0f;
    float m_crossfadeDuration = 0.0f;

    // Active triggers (fixed-size, DLL-safe)
    char  m_activeTriggers[kMaxActiveTriggers][kMaxTriggerNameLen] = {};
    int   m_activeTriggerCount = 0;

    // Callback state for onStateChanged
    bool  m_stateJustChanged = false;
    char  m_prevStateName[kMaxTriggerNameLen] = {};

    // Internal helpers
    int   findState(const char* name) const;
    bool  isTriggerActive(const char* name) const;
    void  removeTrigger(const char* name);
    void  beginTransition(int targetIdx, float duration);
    void  completeTransition();
    bool  isClipFinished(const AnimState& state) const;
    void  advanceState(AnimState& state, float dt) const;
};

} // namespace sv
