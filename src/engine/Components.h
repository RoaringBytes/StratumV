// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <cstdint>

namespace sv {

// ── Transform ───────────────────────────────────────────────────
struct TransformComponent {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    glm::mat4 matrix() const {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
        m *= glm::mat4_cast(rotation);
        m = glm::scale(m, scale);
        return m;
    }
};

// ── Camera ──────────────────────────────────────────────────────
struct CameraComponent {
    float fov    = 60.0f;
    float zNear  = 0.5f;
    float zFar   = 2000.0f;
    bool  active = true;
};

// ── Tag ─────────────────────────────────────────────────────────
struct TagComponent {
    std::string name;
};

// ── Light ───────────────────────────────────────────────────────
enum class LightType : uint8_t {
    Directional = 0,
    Point       = 1,
    Rect        = 2,
    Disk        = 3
};

struct LightComponent {
    LightType type = LightType::Directional;
    glm::vec3 color{1.0f};
    float     intensity = 1.0f;
    float     radius = 0.0f;
    glm::vec2 halfExtents{0.0f};
};

// ── Jolt rigid body (structural simulation) ────────────────────
// Marks entities whose transform is driven by Jolt rigid body simulation.
struct JoltBodyComponent {
    uint32_t bodyId    = ~0u;
    bool     isCompound = false;  // true if created via createCompoundBody
};

// ── Jolt constraint (structural joints) ────────────────────────
struct JoltConstraintComponent {
    uint32_t constraintId = ~0u;
};

// ── Debris particle marker ─────────────────────────────────────
struct DebrisComponent {
    float    lifetime       = 0.f;    // elapsed since spawn
    float    maxLifetime    = 5.f;    // despawn threshold (seconds)
    uint32_t instanceHandle = ~0u;    // GPU instance handle for removal
    uint32_t meshId         = ~0u;    // debris mesh type
    float    mass           = 0.f;    // body mass (for threshold checks)
};

// ── Player ──────────────────────────────────────────────────────
struct PlayerComponent {
    float health = 100.f;
    float stamina = 100.f;
    float hunger = 100.f;

    float walkSpeed  = 8.f;
    float sprintSpeed = 14.f;
    float swimSpeed  = 5.f;
    float jumpForce  = 8.f;
    float gravity    = -20.f;
    float mouseSensitivity = 0.15f;
    float cameraDistance   = 12.f;
    float cameraHeight     = 1.8f;   // eye height above entity root
    float interactRange    = 4.f;
    float groundDamping      = 10.f;   // ground deceleration rate (exp decay, 1/s)
    float swimDamping        = 3.f;    // swim XZ deceleration rate
    float swimVerticalDamping = 6.5f;  // swim Y deceleration rate
    float buoyancyDamping    = 1.2f;   // buoyancy drag rate
    float acceleration       = 12.f;   // acceleration smoothing rate
    bool  firstPerson      = false;

    glm::vec3 velocity{0.f};
    glm::vec3 forward{0.f, 0.f, -1.f};
    glm::vec3 right{1.f, 0.f, 0.f};
    float yaw   = 0.f;
    float pitch = -15.f;

    bool grounded  = false;
    bool swimming  = false;
    bool sprinting = false;

    // ── Swim acceleration ────────
    float swimAcceleration   = 6.f;    // slower ramp in water (vs ground acceleration=12)

    // ── Crouch ───────────────────
    float crouchSpeed            = 4.f;    // m/s while crouched
    float crouchCameraHeight     = 1.0f;   // eye height when crouched (vs 1.8 standing)
    float crouchCapsuleHalfHeight = 0.45f; // PhysX capsule half-height when crouched (vs 0.9)
    float crouchTransitionSpeed  = 8.f;    // exp smoothing rate for crouch lerp
    bool  crouching              = false;  // runtime state
    float crouchFraction         = 0.f;    // runtime: 0=standing, 1=crouched

    // ── Camera smoothing ─────────
    float cameraSmoothingYaw   = 20.f;   // exp smoothing rate (higher = snappier)
    float cameraSmoothingPitch = 20.f;
    float yawSensitivity       = 0.15f;  // separate yaw sensitivity
    float pitchSensitivity     = 0.15f;  // separate pitch sensitivity
    float targetYaw            = 0.f;    // runtime: raw target that yaw chases
    float targetPitch          = -15.f;  // runtime: raw target that pitch chases

    // ── Head bob ─────────────────
    float headBobAmplitude   = 0.035f;   // vertical bob in meters
    float headBobFrequency   = 1.8f;     // cycles/sec while walking
    float headBobSprintMult  = 1.4f;     // frequency multiplier when sprinting
    bool  headBobEnabled     = true;
    float headBobPhase       = 0.f;      // runtime: sine phase (radians)
    float headBobOffset      = 0.f;      // runtime: vertical offset this frame
};

// ── Team ───────────────────────────────────────────────────────
struct TeamComponent {
    uint8_t   teamId = 0;                     // 0 = unassigned
    glm::vec4 teamColor{1.f, 1.f, 1.f, 1.f}; // RGBA team identification
    char      teamName[32] = "Unassigned";    // fixed buffer (DLL-safe)
};

// ── Inventory ──────────────────────────────────────────────────
struct InventorySlot {
    uint16_t itemId = 0;   // 0 = empty
    uint16_t count  = 0;
};

struct InventoryComponent {
    static constexpr int kMaxSlots = 10;
    InventorySlot slots[kMaxSlots] = {};
    int8_t activeSlot = 0;  // selected hotbar slot (0-9)
};

// ── Gold ───────────────────────────────────────────────────────
struct GoldComponent {
    uint32_t gold = 0;
};

} // namespace sv
