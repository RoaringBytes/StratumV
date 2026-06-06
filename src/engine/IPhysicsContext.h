// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// IPhysicsContext — abstract physics interface for StratumV games.
//
// Lives at Layer 4 (Engine Services). Games wire a concrete implementation
// (e.g. JoltPhysicsContext) into BaseSystemContext::physics.
// Use createNoOpPhysicsContext() when physics is not yet implemented.

#include "PhysicsTypes.h"
#include <memory>

namespace sv {

class IPhysicsContext {
public:
    virtual ~IPhysicsContext() = default;

    // ── Lifecycle ───────────────────────────────────────────────────
    virtual void init() = 0;
    virtual void shutdown() = 0;
    virtual bool isInitialized() const = 0;

    // ── Simulation ──────────────────────────────────────────────────
    // Fixed-timestep update (internally accumulates dt, steps at fixed rate).
    virtual void step(float dt) = 0;
    virtual void setGravity(float y) = 0;

    // ── Rigid bodies ────────────────────────────────────────────────
    virtual PhysicsBodyHandle createDynamicBox(const glm::vec3& halfExtents,
                                               const glm::vec3& position,
                                               float mass = 1.0f) = 0;
    virtual PhysicsBodyHandle createStaticBox(const glm::vec3& halfExtents,
                                              const glm::vec3& position) = 0;
    virtual PhysicsBodyHandle createCompoundBody(const glm::vec3* halfExtents,
                                                 const glm::vec3* localPositions,
                                                 const glm::quat* localRotations,
                                                 int partCount,
                                                 const glm::vec3& worldPos,
                                                 float totalMass) = 0;
    virtual void destroyBody(PhysicsBodyHandle h) = 0;

    virtual glm::vec3 getBodyPosition(PhysicsBodyHandle h) const = 0;
    virtual glm::quat getBodyRotation(PhysicsBodyHandle h) const = 0;
    virtual int getBodyCount() const = 0;
    virtual int getActiveBodyCount() const = 0;

    // ── Terrain heightfield ─────────────────────────────────────────
    virtual void createHeightfield(const float* heights, int rows, int cols,
                                   float rowScale, float colScale, float heightScale,
                                   const glm::vec3& origin) = 0;
    virtual void updateHeightfield(const float* heights, int rows, int cols) = 0;

    // ── Character controller ────────────────────────────────────────
    virtual PhysicsCharacterHandle createCharacter(const PhysicsCharacterConfig& cfg,
                                                   const glm::vec3& startPos) = 0;
    virtual void updateCharacter(PhysicsCharacterHandle handle,
                                 const glm::vec3& desiredVelocity, float dt) = 0;
    virtual glm::vec3 getCharacterPosition(PhysicsCharacterHandle handle) const = 0;
    virtual void setCharacterPosition(PhysicsCharacterHandle handle,
                                      const glm::vec3& footPos) = 0;
    virtual bool isCharacterGrounded(PhysicsCharacterHandle handle) const = 0;
    virtual glm::vec3 getCharacterVelocity(PhysicsCharacterHandle handle) const = 0;
    virtual const PhysicsCharacterConfig* getCharacterConfig(PhysicsCharacterHandle handle) const = 0;
    virtual bool resizeCharacter(PhysicsCharacterHandle handle, float newHalfHeight) = 0;

    // ── Parameters ──────────────────────────────────────────────────
    virtual void applyParams(const PhysicsParams& params) = 0;

    // ── Constraints ─────────────────────────────────────────────────
    virtual ConstraintHandle createConstraint(const ConstraintDesc& desc) = 0;
    virtual void             destroyConstraint(ConstraintHandle h) = 0;
    virtual ConstraintInfo   getConstraintInfo(ConstraintHandle h) const = 0;
    virtual bool             isConstraintBroken(ConstraintHandle h) const = 0;
    virtual int              getConstraintCount() const = 0;
    virtual std::vector<ConstraintInfo> getAllConstraintInfo() const = 0;

    virtual void setBreakCallback(PhysicsBreakCallback cb) = 0;
    virtual void setCascadeParams(const CascadeParams& params) = 0;

    // ── Ray cast ────────────────────────────────────────────────────
    virtual PhysicsRayHit raycast(const glm::vec3& origin,
                                  const glm::vec3& direction,
                                  float maxDist) const = 0;

    // ── Diagnostics ─────────────────────────────────────────────────
    virtual float getStepTimeMs() const = 0;  // wall-clock time of last step()
};

// No-op implementation — all creates return invalid handles, all queries return defaults.
std::unique_ptr<IPhysicsContext> createNoOpPhysicsContext();

} // namespace sv
