// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#ifdef _WIN32
#define NOMINMAX
#endif

#include "AnimationStateMachine.h"
#include "CrtCompat.h"

#include <cstdio>
#include <cmath>
#include <algorithm>

namespace sv {

// ── State graph construction ─────────────────────────────────────────

void AnimationStateMachine::addState(const char* name, const AnimationClip* clip,
                                     bool looping, float speed) {
    AnimState state{};
    sv::StrCopy(state.name, name);
    state.clip  = clip;
    state.loop  = looping;
    state.speed = speed;
    m_states.push_back(state);
}

void AnimationStateMachine::addTransition(const char* from, const char* to,
                                          float crossfadeDuration, const char* trigger,
                                          TransitionMode mode) {
    AnimTransition t{};
    sv::StrCopy(t.fromState, from);
    sv::StrCopy(t.toState, to);
    if (trigger && trigger[0] != '\0')
        sv::StrCopy(t.trigger, trigger);
    t.crossfadeDuration = crossfadeDuration;
    t.mode = mode;
    m_transitions.push_back(t);
}

void AnimationStateMachine::setInitialState(const char* name) {
    int idx = findState(name);
    if (idx < 0) {
        printf("[AnimSM] setInitialState: state '%s' not found\n", name);
        return;
    }
    m_currentStateIndex = idx;
    m_states[idx].timeCursor = 0.0f;
    m_targetStateIndex  = -1;
    m_crossfadeElapsed  = 0.0f;
    m_crossfadeDuration = 0.0f;
}

// ── Per-frame update ─────────────────────────────────────────────────

void AnimationStateMachine::update(float dt) {
    if (m_currentStateIndex < 0) return;

    m_stateJustChanged = false;

    if (m_targetStateIndex >= 0) {
        // ── Crossfading ──────────────────────────────────────────
        advanceState(m_states[m_currentStateIndex], dt);
        advanceState(m_states[m_targetStateIndex], dt);

        m_crossfadeElapsed += dt;
        if (m_crossfadeElapsed >= m_crossfadeDuration)
            completeTransition();
    } else {
        // ── Stable state — advance clip and check transitions ────
        advanceState(m_states[m_currentStateIndex], dt);

        // Check for non-looping clip completion
        const auto& cur = m_states[m_currentStateIndex];
        bool clipDone = isClipFinished(cur);

        // Scan transitions from current state
        for (const auto& t : m_transitions) {
            if (strcmp(t.fromState, cur.name) != 0) continue;

            bool shouldFire = false;
            if (t.mode == TransitionMode::Immediate) {
                // Immediate: requires trigger to be active
                if (t.trigger[0] != '\0' && isTriggerActive(t.trigger))
                    shouldFire = true;
            } else { // WaitForEnd
                // WaitForEnd: fires when clip finishes
                if (clipDone) {
                    // If trigger is set, also require it; empty trigger = auto
                    if (t.trigger[0] == '\0' || isTriggerActive(t.trigger))
                        shouldFire = true;
                }
            }

            if (shouldFire) {
                int targetIdx = findState(t.toState);
                if (targetIdx >= 0) {
                    if (t.trigger[0] != '\0')
                        removeTrigger(t.trigger);
                    beginTransition(targetIdx, t.crossfadeDuration);
                    break;
                }
            }
        }
    }
}

// ── Trigger system ───────────────────────────────────────────────────

void AnimationStateMachine::setTrigger(const char* name) {
    if (!name || name[0] == '\0') return;
    if (isTriggerActive(name)) return; // already set

    if (m_activeTriggerCount >= kMaxActiveTriggers) {
        printf("[AnimSM] Trigger overflow — max %d active triggers\n", kMaxActiveTriggers);
        return;
    }

    sv::StrCopy(m_activeTriggers[m_activeTriggerCount], name);
    m_activeTriggerCount++;
}

void AnimationStateMachine::clearTrigger(const char* name) {
    removeTrigger(name);
}

void AnimationStateMachine::clearAllTriggers() {
    m_activeTriggerCount = 0;
    memset(m_activeTriggers, 0, sizeof(m_activeTriggers));
}

// ── Force transition ─────────────────────────────────────────────────

void AnimationStateMachine::triggerTransition(const char* targetState, float duration) {
    int idx = findState(targetState);
    if (idx < 0) {
        printf("[AnimSM] triggerTransition: state '%s' not found\n", targetState);
        return;
    }
    if (idx == m_currentStateIndex && m_targetStateIndex < 0)
        return; // already in that state

    // Look for a matching transition edge for duration
    float dur = (duration >= 0.0f) ? duration : 0.3f; // default crossfade
    if (m_currentStateIndex >= 0 && duration < 0.0f) {
        const char* fromName = m_states[m_currentStateIndex].name;
        for (const auto& t : m_transitions) {
            if (strcmp(t.fromState, fromName) == 0 && strcmp(t.toState, targetState) == 0) {
                dur = t.crossfadeDuration;
                break;
            }
        }
    }

    beginTransition(idx, dur);
}

// ── Query ────────────────────────────────────────────────────────────

const char* AnimationStateMachine::getCurrentStateName() const {
    if (m_currentStateIndex < 0) return "";
    return m_states[m_currentStateIndex].name;
}

bool AnimationStateMachine::isTransitioning() const {
    return m_targetStateIndex >= 0;
}

float AnimationStateMachine::getTransitionProgress() const {
    if (m_targetStateIndex < 0 || m_crossfadeDuration <= 0.0f) return 0.0f;
    return std::min(m_crossfadeElapsed / m_crossfadeDuration, 1.0f);
}

float AnimationStateMachine::getCurrentTime() const {
    if (m_currentStateIndex < 0) return 0.0f;
    return m_states[m_currentStateIndex].timeCursor;
}

float AnimationStateMachine::getCurrentDuration() const {
    if (m_currentStateIndex < 0) return 0.0f;
    const auto& s = m_states[m_currentStateIndex];
    return s.clip ? s.clip->duration : 0.0f;
}

int AnimationStateMachine::getStateCount() const {
    return (int)m_states.size();
}

const AnimState* AnimationStateMachine::getState(int index) const {
    if (index < 0 || index >= (int)m_states.size()) return nullptr;
    return &m_states[index];
}

// ── BlendLayer output ────────────────────────────────────────────────

int AnimationStateMachine::getBlendLayers(BlendLayer* outLayers, int maxLayers) const {
    if (m_currentStateIndex < 0 || maxLayers <= 0) return 0;

    const auto& cur = m_states[m_currentStateIndex];

    if (m_targetStateIndex >= 0 && maxLayers >= 2) {
        // Crossfading: two layers
        float progress = getTransitionProgress();
        // Smoothstep for smoother crossfade
        float t = progress * progress * (3.0f - 2.0f * progress);

        outLayers[0].clip   = cur.clip;
        outLayers[0].weight = 1.0f - t;
        outLayers[0].time   = cur.timeCursor;

        const auto& tgt = m_states[m_targetStateIndex];
        outLayers[1].clip   = tgt.clip;
        outLayers[1].weight = t;
        outLayers[1].time   = tgt.timeCursor;

        return 2;
    }

    // Single state
    outLayers[0].clip   = cur.clip;
    outLayers[0].weight = 1.0f;
    outLayers[0].time   = cur.timeCursor;
    return 1;
}

// ── Internal helpers ─────────────────────────────────────────────────

int AnimationStateMachine::findState(const char* name) const {
    for (int i = 0; i < (int)m_states.size(); i++) {
        if (strcmp(m_states[i].name, name) == 0)
            return i;
    }
    return -1;
}

bool AnimationStateMachine::isTriggerActive(const char* name) const {
    for (int i = 0; i < m_activeTriggerCount; i++) {
        if (strcmp(m_activeTriggers[i], name) == 0)
            return true;
    }
    return false;
}

void AnimationStateMachine::removeTrigger(const char* name) {
    for (int i = 0; i < m_activeTriggerCount; i++) {
        if (strcmp(m_activeTriggers[i], name) == 0) {
            // Swap with last
            if (i < m_activeTriggerCount - 1)
                memcpy(m_activeTriggers[i], m_activeTriggers[m_activeTriggerCount - 1],
                       kMaxTriggerNameLen);
            m_activeTriggers[m_activeTriggerCount - 1][0] = '\0';
            m_activeTriggerCount--;
            return;
        }
    }
}

void AnimationStateMachine::beginTransition(int targetIdx, float duration) {
    if (targetIdx == m_currentStateIndex) return;

    sv::StrCopy(m_prevStateName, m_states[m_currentStateIndex].name);

    m_targetStateIndex  = targetIdx;
    m_crossfadeElapsed  = 0.0f;
    m_crossfadeDuration = std::max(duration, 0.001f); // avoid div-by-zero

    // Reset target clip's time cursor for clean start
    m_states[targetIdx].timeCursor = 0.0f;

    printf("[AnimSM] Transition: '%s' -> '%s' (%.2fs crossfade)\n",
           m_prevStateName, m_states[targetIdx].name, duration);
}

void AnimationStateMachine::completeTransition() {
    m_currentStateIndex = m_targetStateIndex;
    m_targetStateIndex  = -1;
    m_crossfadeElapsed  = 0.0f;
    m_crossfadeDuration = 0.0f;
    m_stateJustChanged  = true;

    printf("[AnimSM] Transition complete -> '%s'\n", m_states[m_currentStateIndex].name);
}

bool AnimationStateMachine::isClipFinished(const AnimState& state) const {
    if (state.loop) return false;
    if (!state.clip) return true;
    return state.timeCursor >= state.clip->duration;
}

void AnimationStateMachine::advanceState(AnimState& state, float dt) const {
    if (!state.clip) return;
    state.timeCursor += dt * state.speed;
    if (state.loop && state.clip->duration > 0.0f) {
        if (state.timeCursor >= state.clip->duration)
            state.timeCursor -= state.clip->duration * std::floor(state.timeCursor / state.clip->duration);
    } else {
        // Non-looping: clamp at duration
        if (state.clip->duration > 0.0f && state.timeCursor > state.clip->duration)
            state.timeCursor = state.clip->duration;
    }
}

} // namespace sv
