// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── S-T1: AnimationStateMachine unit tests ──────────────────────────
// Tests for sv::AnimationStateMachine (AnimationStateMachine.h/.cpp).
//
// Covers:
//  - State graph construction (addState, addTransition, setInitialState)
//  - Trigger system (setTrigger, clearTrigger, clearAllTriggers, overflow)
//  - Transition modes: Immediate vs WaitForEnd
//  - Crossfade progress (smoothstep)
//  - getBlendLayers output (single vs dual layer)
//  - Non-looping clip completion
//  - Missing state + missing trigger handling
//
// Strategy: AnimationClip has `duration` (float) and `animation` (shared_ptr).
// We build dummy clips with duration set but animation == nullptr. The state
// machine only touches clip->duration and clip->loop — it never dereferences
// the ozz Animation — so this is sufficient for logic tests.

#include "AnimationStateMachine.h"
#include "AnimationTypes.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using sv::AnimationStateMachine;
using sv::AnimationClip;
using sv::BlendLayer;
using sv::TransitionMode;
using Catch::Matchers::WithinAbs;

namespace {

// Construct a dummy clip with a given duration. The ozz Animation pointer is
// left null; the state machine never dereferences it.
AnimationClip makeDummyClip(const char* name, float duration) {
    AnimationClip clip;
    clip.name     = name;
    clip.duration = duration;
    clip.animation.reset();
    return clip;
}

} // anonymous

// ── State graph construction ─────────────────────────────────────────

TEST_CASE("AnimSM: addState increments state count", "[anim-sm]") {
    AnimationStateMachine sm;
    REQUIRE(sm.getStateCount() == 0);

    auto idle = makeDummyClip("idle", 1.0f);
    sm.addState("idle", &idle);
    REQUIRE(sm.getStateCount() == 1);

    auto walk = makeDummyClip("walk", 1.2f);
    sm.addState("walk", &walk);
    REQUIRE(sm.getStateCount() == 2);
}

TEST_CASE("AnimSM: setInitialState selects the given state", "[anim-sm]") {
    AnimationStateMachine sm;
    auto idle = makeDummyClip("idle", 1.0f);
    auto walk = makeDummyClip("walk", 1.2f);
    sm.addState("idle", &idle);
    sm.addState("walk", &walk);

    sm.setInitialState("walk");
    REQUIRE(std::string(sm.getCurrentStateName()) == "walk");
}

TEST_CASE("AnimSM: setInitialState with unknown name is a no-op", "[anim-sm]") {
    AnimationStateMachine sm;
    auto idle = makeDummyClip("idle", 1.0f);
    sm.addState("idle", &idle);
    sm.setInitialState("idle");
    sm.setInitialState("nonexistent"); // should not crash, no state change
    REQUIRE(std::string(sm.getCurrentStateName()) == "idle");
}

TEST_CASE("AnimSM: update before setInitialState is a no-op", "[anim-sm]") {
    AnimationStateMachine sm;
    auto idle = makeDummyClip("idle", 1.0f);
    sm.addState("idle", &idle);

    // No initial state set — update should do nothing
    sm.update(0.1f);
    REQUIRE(std::string(sm.getCurrentStateName()) == "");
}

// ── Stable-state clip advance ────────────────────────────────────────

TEST_CASE("AnimSM: stable state advances time cursor", "[anim-sm]") {
    AnimationStateMachine sm;
    auto idle = makeDummyClip("idle", 2.0f);
    sm.addState("idle", &idle);
    sm.setInitialState("idle");

    REQUIRE_THAT(sm.getCurrentTime(), WithinAbs(0.0f, 1e-6f));
    sm.update(0.5f);
    REQUIRE_THAT(sm.getCurrentTime(), WithinAbs(0.5f, 1e-6f));
    sm.update(0.3f);
    REQUIRE_THAT(sm.getCurrentTime(), WithinAbs(0.8f, 1e-6f));
}

TEST_CASE("AnimSM: looping clip wraps time", "[anim-sm]") {
    AnimationStateMachine sm;
    auto idle = makeDummyClip("idle", 1.0f);
    sm.addState("idle", &idle, /*looping=*/true);
    sm.setInitialState("idle");

    sm.update(1.5f); // past duration 1.0 → should wrap to 0.5
    REQUIRE_THAT(sm.getCurrentTime(), WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("AnimSM: non-looping clip clamps at duration", "[anim-sm]") {
    AnimationStateMachine sm;
    auto attack = makeDummyClip("attack", 0.5f);
    sm.addState("attack", &attack, /*looping=*/false);
    sm.setInitialState("attack");

    sm.update(5.0f); // way past duration — should clamp
    REQUIRE_THAT(sm.getCurrentTime(), WithinAbs(0.5f, 1e-5f));
}

// ── Trigger system ───────────────────────────────────────────────────

TEST_CASE("AnimSM: Immediate transition fires on trigger", "[anim-sm]") {
    AnimationStateMachine sm;
    auto idle = makeDummyClip("idle", 1.0f);
    auto walk = makeDummyClip("walk", 1.0f);
    sm.addState("idle", &idle);
    sm.addState("walk", &walk);
    sm.addTransition("idle", "walk", 0.2f, "run", TransitionMode::Immediate);
    sm.setInitialState("idle");

    // No trigger yet — no transition
    sm.update(0.1f);
    REQUIRE(std::string(sm.getCurrentStateName()) == "idle");
    REQUIRE(!sm.isTransitioning());

    // Fire the trigger — transition begins next update
    sm.setTrigger("run");
    sm.update(0.01f);
    REQUIRE(sm.isTransitioning());
}

TEST_CASE("AnimSM: trigger is consumed by transition", "[anim-sm]") {
    AnimationStateMachine sm;
    auto idle = makeDummyClip("idle", 1.0f);
    auto walk = makeDummyClip("walk", 1.0f);
    auto run  = makeDummyClip("run",  1.0f);
    sm.addState("idle", &idle);
    sm.addState("walk", &walk);
    sm.addState("run",  &run);
    sm.addTransition("idle", "walk", 0.1f, "go", TransitionMode::Immediate);
    sm.addTransition("walk", "run",  0.1f, "go", TransitionMode::Immediate);
    sm.setInitialState("idle");

    sm.setTrigger("go");
    // First update: idle -> walk begins
    sm.update(0.01f);
    REQUIRE(sm.isTransitioning());

    // Complete the crossfade (> 0.1s total)
    sm.update(0.2f);
    REQUIRE(std::string(sm.getCurrentStateName()) == "walk");

    // Trigger was consumed; walk -> run should NOT auto-fire
    sm.update(0.2f);
    REQUIRE(std::string(sm.getCurrentStateName()) == "walk");
    REQUIRE(!sm.isTransitioning());
}

TEST_CASE("AnimSM: clearAllTriggers prevents transitions", "[anim-sm]") {
    AnimationStateMachine sm;
    auto idle = makeDummyClip("idle", 1.0f);
    auto walk = makeDummyClip("walk", 1.0f);
    sm.addState("idle", &idle);
    sm.addState("walk", &walk);
    sm.addTransition("idle", "walk", 0.2f, "go", TransitionMode::Immediate);
    sm.setInitialState("idle");

    sm.setTrigger("go");
    sm.clearAllTriggers();
    sm.update(0.01f);
    REQUIRE(std::string(sm.getCurrentStateName()) == "idle");
    REQUIRE(!sm.isTransitioning());
}

TEST_CASE("AnimSM: setTrigger is idempotent", "[anim-sm]") {
    AnimationStateMachine sm;
    auto idle = makeDummyClip("idle", 1.0f);
    sm.addState("idle", &idle);
    sm.setInitialState("idle");

    // Setting same trigger multiple times should not overflow
    for (int i = 0; i < 20; ++i) {
        sm.setTrigger("duplicate");
    }
    // No crash, still in idle (no transition from idle for this trigger)
    REQUIRE(std::string(sm.getCurrentStateName()) == "idle");
}

// ── WaitForEnd transition ────────────────────────────────────────────

TEST_CASE("AnimSM: WaitForEnd auto-transitions when clip finishes", "[anim-sm]") {
    AnimationStateMachine sm;
    auto attack = makeDummyClip("attack", 0.5f);
    auto idle   = makeDummyClip("idle",   1.0f);
    sm.addState("attack", &attack, /*looping=*/false);
    sm.addState("idle",   &idle);
    // Empty trigger + WaitForEnd = auto transition
    sm.addTransition("attack", "idle", 0.1f, "", TransitionMode::WaitForEnd);
    sm.setInitialState("attack");

    // Advance less than duration — still in attack
    sm.update(0.3f);
    REQUIRE(std::string(sm.getCurrentStateName()) == "attack");

    // Advance past duration — transition should fire
    sm.update(0.3f); // now 0.6 > 0.5
    REQUIRE(sm.isTransitioning());
}

TEST_CASE("AnimSM: WaitForEnd with looping clip never auto-fires", "[anim-sm]") {
    AnimationStateMachine sm;
    auto loop = makeDummyClip("loop", 0.5f);
    auto idle = makeDummyClip("idle", 1.0f);
    sm.addState("loop", &loop, /*looping=*/true);
    sm.addState("idle", &idle);
    sm.addTransition("loop", "idle", 0.1f, "", TransitionMode::WaitForEnd);
    sm.setInitialState("loop");

    for (int i = 0; i < 20; ++i)
        sm.update(0.1f);

    // Looping clip never finishes — no transition
    REQUIRE(std::string(sm.getCurrentStateName()) == "loop");
    REQUIRE(!sm.isTransitioning());
}

// ── Crossfade progress + smoothstep ──────────────────────────────────

TEST_CASE("AnimSM: getTransitionProgress advances with time", "[anim-sm]") {
    AnimationStateMachine sm;
    auto a = makeDummyClip("a", 1.0f);
    auto b = makeDummyClip("b", 1.0f);
    sm.addState("a", &a);
    sm.addState("b", &b);
    sm.addTransition("a", "b", 1.0f, "go", TransitionMode::Immediate);
    sm.setInitialState("a");

    sm.setTrigger("go");
    sm.update(0.01f); // transition begins
    REQUIRE(sm.isTransitioning());
    REQUIRE_THAT(sm.getTransitionProgress(), WithinAbs(0.01f, 0.02f));

    sm.update(0.49f); // midpoint
    REQUIRE_THAT(sm.getTransitionProgress(), WithinAbs(0.5f, 0.02f));

    sm.update(0.6f); // past end — should complete
    REQUIRE(!sm.isTransitioning());
}

TEST_CASE("AnimSM: crossfade blend layers produce smoothstep weights", "[anim-sm]") {
    AnimationStateMachine sm;
    auto a = makeDummyClip("a", 1.0f);
    auto b = makeDummyClip("b", 1.0f);
    sm.addState("a", &a);
    sm.addState("b", &b);
    sm.addTransition("a", "b", 1.0f, "go", TransitionMode::Immediate);
    sm.setInitialState("a");

    // Two updates: the first begins the transition (crossfadeElapsed = 0),
    // the second advances the crossfade into the middle.
    sm.setTrigger("go");
    sm.update(0.001f); // begins transition
    sm.update(0.5f);   // advances crossfade to 0.5 / 1.0 = 0.5

    BlendLayer layers[2] = {};
    int count = sm.getBlendLayers(layers, 2);
    REQUIRE(count == 2);

    // smoothstep(0.5) = 0.5^2 * (3 - 2*0.5) = 0.25 * 2 = 0.5 exactly
    REQUIRE_THAT(layers[0].weight, WithinAbs(0.5f, 1e-3f));
    REQUIRE_THAT(layers[1].weight, WithinAbs(0.5f, 1e-3f));

    // Layer sources are the two clips
    REQUIRE(layers[0].clip == &a);
    REQUIRE(layers[1].clip == &b);
}

TEST_CASE("AnimSM: stable state returns a single blend layer", "[anim-sm]") {
    AnimationStateMachine sm;
    auto a = makeDummyClip("a", 1.0f);
    sm.addState("a", &a);
    sm.setInitialState("a");
    sm.update(0.1f);

    BlendLayer layers[2] = {};
    int count = sm.getBlendLayers(layers, 2);
    REQUIRE(count == 1);
    REQUIRE_THAT(layers[0].weight, WithinAbs(1.0f, 1e-6f));
    REQUIRE(layers[0].clip == &a);
}

// ── Force transition ─────────────────────────────────────────────────

TEST_CASE("AnimSM: triggerTransition forces immediate transition", "[anim-sm]") {
    AnimationStateMachine sm;
    auto a = makeDummyClip("a", 1.0f);
    auto b = makeDummyClip("b", 1.0f);
    sm.addState("a", &a);
    sm.addState("b", &b);
    sm.setInitialState("a");

    // No transition edge exists — triggerTransition bypasses the graph
    sm.triggerTransition("b", 0.1f);
    REQUIRE(sm.isTransitioning());

    // Advance past crossfade
    sm.update(0.2f);
    REQUIRE(std::string(sm.getCurrentStateName()) == "b");
}

TEST_CASE("AnimSM: triggerTransition to missing state is a no-op", "[anim-sm]") {
    AnimationStateMachine sm;
    auto a = makeDummyClip("a", 1.0f);
    sm.addState("a", &a);
    sm.setInitialState("a");

    sm.triggerTransition("nonexistent", 0.1f);
    REQUIRE(!sm.isTransitioning());
    REQUIRE(std::string(sm.getCurrentStateName()) == "a");
}
