// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// PhysicsTypes — engine-level physics value types.
//
// Lives at Layer 4 (Engine Services). Used by IPhysicsContext and consumers.
// Game-specific types (FracturePattern,
// GpuInstanceHandle) remain in the consumer.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <functional>
#include <vector>

namespace sv {

// ── Live-tunable physics parameters ─────────────────────────────
struct PhysicsParams {
    float gravity         = -9.81f;  // Y-axis magnitude (negative = down)
    float friction        = 0.6f;
    float restitution     = 0.1f;
    float capsuleRadius   = 0.35f;
    float capsuleHalfHeight = 0.9f;
    float maxSlopeAngle   = 50.0f;   // degrees
    float stepHeight      = 0.35f;
    float contactOffset   = 0.08f;
};

// ── Raycast result ──────────────────────────────────────────────
struct PhysicsRayHit {
    glm::vec3 position{0};
    glm::vec3 normal{0, 1, 0};
    float distance = 0;
    bool hit = false;
};

// ── Body handle ─────────────────────────────────────────────────
struct PhysicsBodyHandle {
    uint32_t id = ~0u;
    bool valid() const { return id != ~0u; }
};

// ── Character controller ────────────────────────────────────────
struct PhysicsCharacterConfig {
    float capsuleRadius     = 0.35f;
    float capsuleHalfHeight = 0.9f;   // half of the cylinder (total height = 2*h + 2*r)
    float maxSlopeAngleDeg  = 50.f;
    float stepHeight        = 0.35f;
    float contactOffset     = 0.08f;
};

struct PhysicsCharacterHandle {
    int  index = -1;
    bool valid() const { return index >= 0; }
};

// ── Constraint types ────────────────────────────────────────────
enum class ConstraintType : uint8_t {
    Fixed,      // rigid weld between two bodies
    Point,      // ball-and-socket joint
    Hinge,      // single-axis rotation
    Distance,   // spring/rod between anchor points
};

struct ConstraintHandle {
    uint32_t id = ~0u;
    bool valid() const { return id != ~0u; }
};

struct ConstraintDesc {
    ConstraintType type       = ConstraintType::Fixed;
    uint32_t       bodyIdA    = ~0u;
    uint32_t       bodyIdB    = ~0u;
    glm::vec3      anchorA    {0.f};     // world-space anchor on body A
    glm::vec3      anchorB    {0.f};     // world-space anchor on body B
    glm::vec3      axisA      {0,1,0};   // hinge axis on body A (Hinge only)
    glm::vec3      axisB      {0,1,0};   // hinge axis on body B (Hinge only)
    float          breakForce = 0.f;     // impulse threshold; 0 = unbreakable
    float          minDistance = -1.f;   // Distance constraint range (-1 = auto)
    float          maxDistance = -1.f;
};

struct ConstraintInfo {
    ConstraintHandle handle;
    ConstraintType   type;
    uint32_t         bodyIdA;
    uint32_t         bodyIdB;
    float            breakForce;
    float            currentImpulse;     // last-frame impulse magnitude
    bool             broken;
};

// ── Cascade break parameters ────────────────────────────────────
struct CascadeParams {
    float stressMultiplier = 1.5f;  // neighbor impulse amplification on break
    int   maxDepth         = 3;     // max cascade waves per substep
    bool  enabled          = true;
};

// ── Break callback ──────────────────────────────────────────────
using PhysicsBreakCallback = std::function<void(ConstraintHandle, uint32_t bodyA, uint32_t bodyB)>;

} // namespace sv
