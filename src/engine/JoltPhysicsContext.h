// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// JoltPhysicsContext — concrete IPhysicsContext backed by Jolt Physics 5.2.
//
// Lives at Layer 4 (Engine Services). Only compiled when STRATUMV_JOLT_AVAILABLE=1.
// Uses pimpl to keep Jolt headers out of the engine interface.

#include "IPhysicsContext.h"
#include <memory>

namespace sv {

class JoltPhysicsContext final : public IPhysicsContext {
public:
    JoltPhysicsContext();
    ~JoltPhysicsContext() override;

    // Non-copyable
    JoltPhysicsContext(const JoltPhysicsContext&) = delete;
    JoltPhysicsContext& operator=(const JoltPhysicsContext&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────
    void init() override;
    void shutdown() override;
    bool isInitialized() const override;

    // ── Simulation ──────────────────────────────────────────────────
    void step(float dt) override;
    void setGravity(float y) override;

    // ── Rigid bodies ────────────────────────────────────────────────
    PhysicsBodyHandle createDynamicBox(const glm::vec3& halfExtents,
                                       const glm::vec3& position,
                                       float mass = 1.0f) override;
    PhysicsBodyHandle createStaticBox(const glm::vec3& halfExtents,
                                      const glm::vec3& position) override;
    PhysicsBodyHandle createCompoundBody(const glm::vec3* halfExtents,
                                         const glm::vec3* localPositions,
                                         const glm::quat* localRotations,
                                         int partCount,
                                         const glm::vec3& worldPos,
                                         float totalMass) override;
    void destroyBody(PhysicsBodyHandle h) override;
    glm::vec3 getBodyPosition(PhysicsBodyHandle h) const override;
    glm::quat getBodyRotation(PhysicsBodyHandle h) const override;
    int getBodyCount() const override;
    int getActiveBodyCount() const override;

    // ── Terrain heightfield ─────────────────────────────────────────
    void createHeightfield(const float* heights, int rows, int cols,
                           float rowScale, float colScale, float heightScale,
                           const glm::vec3& origin) override;
    void updateHeightfield(const float* heights, int rows, int cols) override;

    // ── Character controller ────────────────────────────────────────
    PhysicsCharacterHandle createCharacter(const PhysicsCharacterConfig& cfg,
                                           const glm::vec3& startPos) override;
    void updateCharacter(PhysicsCharacterHandle handle,
                         const glm::vec3& desiredVelocity, float dt) override;
    glm::vec3 getCharacterPosition(PhysicsCharacterHandle handle) const override;
    void setCharacterPosition(PhysicsCharacterHandle handle,
                              const glm::vec3& footPos) override;
    bool isCharacterGrounded(PhysicsCharacterHandle handle) const override;
    glm::vec3 getCharacterVelocity(PhysicsCharacterHandle handle) const override;
    const PhysicsCharacterConfig* getCharacterConfig(PhysicsCharacterHandle handle) const override;
    bool resizeCharacter(PhysicsCharacterHandle handle, float newHalfHeight) override;

    // ── Parameters ──────────────────────────────────────────────────
    void applyParams(const PhysicsParams& params) override;

    // ── Constraints ─────────────────────────────────────────────────
    ConstraintHandle createConstraint(const ConstraintDesc& desc) override;
    void             destroyConstraint(ConstraintHandle h) override;
    ConstraintInfo   getConstraintInfo(ConstraintHandle h) const override;
    bool             isConstraintBroken(ConstraintHandle h) const override;
    int              getConstraintCount() const override;
    std::vector<ConstraintInfo> getAllConstraintInfo() const override;
    void setBreakCallback(PhysicsBreakCallback cb) override;
    void setCascadeParams(const CascadeParams& params) override;

    // ── Ray cast ────────────────────────────────────────────────────
    PhysicsRayHit raycast(const glm::vec3& origin,
                          const glm::vec3& direction,
                          float maxDist) const override;

    // ── Diagnostics ─────────────────────────────────────────────────
    float getStepTimeMs() const override;

private:
    void checkBreakableConstraints();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sv
