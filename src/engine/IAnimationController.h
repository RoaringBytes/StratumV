// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// Per-entity animation controller interface.
// Game DLL plugins implement this to drive animation state machines.
// NOT an IModularSystem — this is a per-entity behavior, not a system plugin.
//
// Attach to entities via AnimatorComponent::controller.
// Engine calls updateAnimation() each frame before the state machine update.
//
// DLL-safe: all parameters are const char*, forward-declared types, or EnTT handles.
// No std::string or std::function crosses the boundary.

#include <entt/entity/registry.hpp>

namespace sv {

class AnimationStateMachine; // forward

class IAnimationController {
public:
    virtual ~IAnimationController() = default;

    // Called each frame before state machine update.
    // Games set triggers, adjust speeds, etc.
    virtual void updateAnimation(float dt, AnimationStateMachine& sm,
                                 entt::entity entity, entt::registry& ecs) = 0;

    // Called when a state transition completes.
    virtual void onStateChanged(const char* prevState, const char* newState,
                                entt::entity entity) {}

    // Called when a non-looping animation reaches its end.
    virtual void onAnimationComplete(const char* stateName,
                                     entt::entity entity) {}
};

} // namespace sv
