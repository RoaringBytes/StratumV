// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "IPhysicsContext.h"

namespace sv {

// ── No-op implementation ────────────────────────────────────────
// All creates return invalid handles; all queries return zeroes/defaults.
// Used when physics is not yet wired into a game.

class NoOpPhysicsContext final : public IPhysicsContext {
public:
    void init() override {}
    void shutdown() override {}
    bool isInitialized() const override { return false; }

    void step(float) override {}
    void setGravity(float) override {}

    PhysicsBodyHandle createDynamicBox(const glm::vec3&, const glm::vec3&, float) override { return {}; }
    PhysicsBodyHandle createStaticBox(const glm::vec3&, const glm::vec3&) override { return {}; }
    PhysicsBodyHandle createCompoundBody(const glm::vec3*, const glm::vec3*, const glm::quat*,
                                         int, const glm::vec3&, float) override { return {}; }
    void destroyBody(PhysicsBodyHandle) override {}
    glm::vec3 getBodyPosition(PhysicsBodyHandle) const override { return glm::vec3(0.f); }
    glm::quat getBodyRotation(PhysicsBodyHandle) const override { return glm::quat(1.f, 0.f, 0.f, 0.f); }
    int getBodyCount() const override { return 0; }
    int getActiveBodyCount() const override { return 0; }

    void createHeightfield(const float*, int, int, float, float, float, const glm::vec3&) override {}
    void updateHeightfield(const float*, int, int) override {}

    PhysicsCharacterHandle createCharacter(const PhysicsCharacterConfig&, const glm::vec3&) override { return {}; }
    void updateCharacter(PhysicsCharacterHandle, const glm::vec3&, float) override {}
    glm::vec3 getCharacterPosition(PhysicsCharacterHandle) const override { return glm::vec3(0.f); }
    void setCharacterPosition(PhysicsCharacterHandle, const glm::vec3&) override {}
    bool isCharacterGrounded(PhysicsCharacterHandle) const override { return false; }
    glm::vec3 getCharacterVelocity(PhysicsCharacterHandle) const override { return glm::vec3(0.f); }
    const PhysicsCharacterConfig* getCharacterConfig(PhysicsCharacterHandle) const override { return nullptr; }
    bool resizeCharacter(PhysicsCharacterHandle, float) override { return false; }

    void applyParams(const PhysicsParams&) override {}

    ConstraintHandle createConstraint(const ConstraintDesc&) override { return {}; }
    void destroyConstraint(ConstraintHandle) override {}
    ConstraintInfo getConstraintInfo(ConstraintHandle) const override { return {}; }
    bool isConstraintBroken(ConstraintHandle) const override { return false; }
    int getConstraintCount() const override { return 0; }
    std::vector<ConstraintInfo> getAllConstraintInfo() const override { return {}; }
    void setBreakCallback(PhysicsBreakCallback) override {}
    void setCascadeParams(const CascadeParams&) override {}

    PhysicsRayHit raycast(const glm::vec3&, const glm::vec3&, float) const override { return {}; }

    float getStepTimeMs() const override { return 0.f; }
};

std::unique_ptr<IPhysicsContext> createNoOpPhysicsContext()
{
    return std::make_unique<NoOpPhysicsContext>();
}

} // namespace sv
