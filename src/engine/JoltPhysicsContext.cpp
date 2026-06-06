// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "JoltPhysicsContext.h"

// Jolt includes — only in this .cpp (pimpl)
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>

#include <cstdio>
#include <cmath>
#include <thread>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <chrono>

using namespace JPH;

namespace sv {

// ── Broad phase layers ──────────────────────────────────────────
namespace JoltLayers {
    static constexpr BroadPhaseLayer NON_MOVING(0);
    static constexpr BroadPhaseLayer MOVING(1);
    static constexpr uint32_t NUM_BROAD_PHASE_LAYERS = 2;
}

namespace JoltObjLayers {
    static constexpr ObjectLayer NON_MOVING = 0;
    static constexpr ObjectLayer MOVING     = 1;
    static constexpr uint32_t NUM_LAYERS    = 2;
}

class JoltBPLayerInterface final : public BroadPhaseLayerInterface {
public:
    uint GetNumBroadPhaseLayers() const override { return JoltLayers::NUM_BROAD_PHASE_LAYERS; }

    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override {
        switch (inLayer) {
            case JoltObjLayers::NON_MOVING: return JoltLayers::NON_MOVING;
            case JoltObjLayers::MOVING:     return JoltLayers::MOVING;
            default:                        return JoltLayers::MOVING;
        }
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override {
        switch ((BroadPhaseLayer::Type)inLayer) {
            case (BroadPhaseLayer::Type)0: return "NON_MOVING";
            case (BroadPhaseLayer::Type)1: return "MOVING";
            default: return "UNKNOWN";
        }
    }
#endif
};

class JoltObjectVsBPFilter final : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer inLayer1, BroadPhaseLayer inLayer2) const override {
        switch (inLayer1) {
            case JoltObjLayers::NON_MOVING:
                return inLayer2 == JoltLayers::MOVING;
            case JoltObjLayers::MOVING:
                return true;
            default:
                return false;
        }
    }
};

class JoltObjectLayerPairFilter final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer inLayer1, ObjectLayer inLayer2) const override {
        switch (inLayer1) {
            case JoltObjLayers::NON_MOVING:
                return inLayer2 == JoltObjLayers::MOVING;
            case JoltObjLayers::MOVING:
                return true;
            default:
                return false;
        }
    }
};

// ── Pimpl ───────────────────────────────────────────────────────
struct JoltPhysicsContext::Impl {
    static constexpr float FIXED_STEP = 1.0f / 60.0f;
    static constexpr int   MAX_SUBSTEPS = 4;

    // Jolt subsystems (owned)
    TempAllocatorImpl*      tempAllocator = nullptr;
    JobSystemThreadPool*    jobSystem     = nullptr;
    PhysicsSystem*          physicsSystem = nullptr;

    // Layer interfaces (must outlive PhysicsSystem)
    JoltBPLayerInterface         bpLayerInterface;
    JoltObjectVsBPFilter         objVsBPFilter;
    JoltObjectLayerPairFilter    objLayerPairFilter;

    float accumulator = 0.0f;
    bool  initialized = false;
    float lastStepMs  = 0.0f;

    // ── Terrain heightfield ─────────────────────────────────────
    BodyID terrainBodyId;
    bool   terrainValid = false;
    float  hfRowScale   = 0.f;
    float  hfColScale   = 0.f;
    float  hfHeightScale = 1.f;
    glm::vec3 hfOrigin {0.f};

    // ── Character controllers ───────────────────────────────────
    struct CharacterState {
        Ref<CharacterVirtual>     character;
        PhysicsCharacterConfig    config;
        glm::vec3                 velocity  {0.f};
        bool                      grounded  = false;
    };
    std::vector<CharacterState> characters;

    // ── Constraints ─────────────────────────────────────────────
    struct ConstraintState {
        Ref<Constraint>     joltConstraint;
        ConstraintDesc      desc;
        float               lastImpulse = 0.f;
        bool                broken      = false;
    };
    std::unordered_map<uint32_t, ConstraintState> constraints;
    uint32_t nextConstraintId = 1;

    // Body → constraint adjacency for cascade propagation
    std::unordered_map<uint32_t, std::vector<uint32_t>> bodyConstraintMap;
    CascadeParams cascadeParams;

    PhysicsBreakCallback breakCallback;
};

// ── Lifetime ────────────────────────────────────────────────────
JoltPhysicsContext::JoltPhysicsContext() : m_impl(std::make_unique<Impl>()) {}
JoltPhysicsContext::~JoltPhysicsContext() { shutdown(); }

bool JoltPhysicsContext::isInitialized() const { return m_impl->initialized; }

void JoltPhysicsContext::init()
{
    if (m_impl->initialized) return;
    auto& d = *m_impl;

    RegisterDefaultAllocator();
    Factory::sInstance = new Factory();
    RegisterTypes();

    d.tempAllocator = new TempAllocatorImpl(16 * 1024 * 1024);

    int numThreads = std::max(1, (int)std::thread::hardware_concurrency() - 1);
    d.jobSystem = new JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, numThreads);

    d.physicsSystem = new PhysicsSystem();
    d.physicsSystem->Init(4096, 0, 4096, 4096,
                          d.bpLayerInterface,
                          d.objVsBPFilter,
                          d.objLayerPairFilter);

    d.physicsSystem->SetGravity(Vec3(0.0f, -9.81f, 0.0f));

    d.initialized = true;
    printf("[Jolt] Initialized: %d worker threads, gravity=(0, -9.81, 0)\n", numThreads);
}

void JoltPhysicsContext::shutdown()
{
    if (!m_impl->initialized) return;
    auto& d = *m_impl;

    // Release constraints
    if (d.physicsSystem) {
        for (auto& [id, cs] : d.constraints) {
            if (cs.joltConstraint)
                d.physicsSystem->RemoveConstraint(cs.joltConstraint);
        }
    }
    d.constraints.clear();
    d.bodyConstraintMap.clear();

    // Release character controllers
    d.characters.clear();

    // Remove all bodies
    if (d.physicsSystem) {
        BodyInterface& bodyInterface = d.physicsSystem->GetBodyInterface();
        BodyIDVector bodyIDs;
        d.physicsSystem->GetBodies(bodyIDs);
        for (const BodyID& id : bodyIDs) {
            bodyInterface.RemoveBody(id);
            bodyInterface.DestroyBody(id);
        }
    }
    d.terrainValid = false;

    delete d.physicsSystem;  d.physicsSystem = nullptr;
    delete d.jobSystem;      d.jobSystem     = nullptr;
    delete d.tempAllocator;  d.tempAllocator = nullptr;

    UnregisterTypes();
    delete Factory::sInstance;
    Factory::sInstance = nullptr;

    d.initialized = false;
    printf("[Jolt] Shutdown\n");
}

// ── Simulation ──────────────────────────────────────────────────
void JoltPhysicsContext::step(float dt)
{
    if (!m_impl->initialized) return;
    auto& d = *m_impl;

    auto t0 = std::chrono::high_resolution_clock::now();

    d.accumulator += dt;
    int steps = 0;
    while (d.accumulator >= Impl::FIXED_STEP && steps < Impl::MAX_SUBSTEPS) {
        d.physicsSystem->Update(Impl::FIXED_STEP, 1, d.tempAllocator, d.jobSystem);
        checkBreakableConstraints();
        d.accumulator -= Impl::FIXED_STEP;
        ++steps;
    }
    if (d.accumulator > Impl::FIXED_STEP)
        d.accumulator = Impl::FIXED_STEP;

    auto t1 = std::chrono::high_resolution_clock::now();
    d.lastStepMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
}

void JoltPhysicsContext::setGravity(float y)
{
    if (!m_impl->initialized) return;
    m_impl->physicsSystem->SetGravity(Vec3(0.0f, y, 0.0f));
}

float JoltPhysicsContext::getStepTimeMs() const { return m_impl->lastStepMs; }

// ── Rigid bodies ────────────────────────────────────────────────
PhysicsBodyHandle JoltPhysicsContext::createDynamicBox(const glm::vec3& halfExtents,
                                                       const glm::vec3& position,
                                                       float mass)
{
    if (!m_impl->initialized) return {};

    BoxShapeSettings shapeSettings(Vec3(halfExtents.x, halfExtents.y, halfExtents.z));
    ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        fprintf(stderr, "[Jolt] Failed to create box shape: %s\n", shapeResult.GetError().c_str());
        return {};
    }

    BodyCreationSettings bodySettings(
        shapeResult.Get(),
        RVec3(position.x, position.y, position.z),
        Quat::sIdentity(),
        EMotionType::Dynamic,
        JoltObjLayers::MOVING
    );
    bodySettings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
    bodySettings.mMassPropertiesOverride.mMass = mass;

    BodyInterface& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
    Body* body = bodyInterface.CreateBody(bodySettings);
    if (!body) {
        fprintf(stderr, "[Jolt] Failed to create dynamic body\n");
        return {};
    }

    bodyInterface.AddBody(body->GetID(), EActivation::Activate);
    return { body->GetID().GetIndexAndSequenceNumber() };
}

PhysicsBodyHandle JoltPhysicsContext::createStaticBox(const glm::vec3& halfExtents,
                                                      const glm::vec3& position)
{
    if (!m_impl->initialized) return {};

    BoxShapeSettings shapeSettings(Vec3(halfExtents.x, halfExtents.y, halfExtents.z));
    ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        fprintf(stderr, "[Jolt] Failed to create box shape: %s\n", shapeResult.GetError().c_str());
        return {};
    }

    BodyCreationSettings bodySettings(
        shapeResult.Get(),
        RVec3(position.x, position.y, position.z),
        Quat::sIdentity(),
        EMotionType::Static,
        JoltObjLayers::NON_MOVING
    );

    BodyInterface& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
    Body* body = bodyInterface.CreateBody(bodySettings);
    if (!body) {
        fprintf(stderr, "[Jolt] Failed to create static body\n");
        return {};
    }

    bodyInterface.AddBody(body->GetID(), EActivation::DontActivate);
    return { body->GetID().GetIndexAndSequenceNumber() };
}

PhysicsBodyHandle JoltPhysicsContext::createCompoundBody(
    const glm::vec3* halfExtents,
    const glm::vec3* localPositions,
    const glm::quat* localRotations,
    int partCount,
    const glm::vec3& worldPos, float totalMass)
{
    if (!m_impl->initialized || partCount <= 0) return {};

    StaticCompoundShapeSettings compoundSettings;
    for (int i = 0; i < partCount; ++i) {
        auto boxShape = new BoxShape(Vec3(halfExtents[i].x, halfExtents[i].y, halfExtents[i].z));
        Vec3 pos(localPositions[i].x, localPositions[i].y, localPositions[i].z);
        JPH::Quat rot(localRotations[i].x, localRotations[i].y, localRotations[i].z, localRotations[i].w);
        compoundSettings.AddShape(pos, rot, boxShape);
    }

    ShapeSettings::ShapeResult shapeResult = compoundSettings.Create();
    if (shapeResult.HasError()) {
        fprintf(stderr, "[Jolt] Failed to create compound shape: %s\n", shapeResult.GetError().c_str());
        return {};
    }

    BodyCreationSettings bodySettings(
        shapeResult.Get(),
        RVec3(worldPos.x, worldPos.y, worldPos.z),
        JPH::Quat::sIdentity(),
        EMotionType::Dynamic,
        JoltObjLayers::MOVING
    );
    bodySettings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
    bodySettings.mMassPropertiesOverride.mMass = totalMass;

    BodyInterface& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
    Body* body = bodyInterface.CreateBody(bodySettings);
    if (!body) {
        fprintf(stderr, "[Jolt] Failed to create compound body\n");
        return {};
    }

    bodyInterface.AddBody(body->GetID(), EActivation::Activate);
    return { body->GetID().GetIndexAndSequenceNumber() };
}

void JoltPhysicsContext::destroyBody(PhysicsBodyHandle h)
{
    if (!m_impl->initialized || !h.valid()) return;

    BodyInterface& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
    BodyID bodyId(h.id);
    bodyInterface.RemoveBody(bodyId);
    bodyInterface.DestroyBody(bodyId);
}

glm::vec3 JoltPhysicsContext::getBodyPosition(PhysicsBodyHandle h) const
{
    if (!m_impl->initialized || !h.valid()) return glm::vec3(0.f);

    BodyInterface& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
    RVec3 pos = bodyInterface.GetPosition(BodyID(h.id));
    return glm::vec3((float)pos.GetX(), (float)pos.GetY(), (float)pos.GetZ());
}

glm::quat JoltPhysicsContext::getBodyRotation(PhysicsBodyHandle h) const
{
    if (!m_impl->initialized || !h.valid()) return glm::quat(1.f, 0.f, 0.f, 0.f);

    BodyInterface& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
    JPH::Quat rot = bodyInterface.GetRotation(BodyID(h.id));
    return glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
}

int JoltPhysicsContext::getBodyCount() const
{
    if (!m_impl->initialized) return 0;
    return (int)m_impl->physicsSystem->GetNumBodies();
}

int JoltPhysicsContext::getActiveBodyCount() const
{
    if (!m_impl->initialized) return 0;
    return (int)m_impl->physicsSystem->GetNumActiveBodies(EBodyType::RigidBody);
}

// ── Terrain heightfield ─────────────────────────────────────────
void JoltPhysicsContext::createHeightfield(const float* heights, int rows, int cols,
                                           float rowScale, float colScale, float heightScale,
                                           const glm::vec3& origin)
{
    if (!m_impl->initialized) return;
    auto& d = *m_impl;

    d.hfRowScale    = rowScale;
    d.hfColScale    = colScale;
    d.hfHeightScale = heightScale;
    d.hfOrigin      = origin;

    // Remove existing terrain body
    if (d.terrainValid) {
        BodyInterface& bodyInterface = d.physicsSystem->GetBodyInterface();
        bodyInterface.RemoveBody(d.terrainBodyId);
        bodyInterface.DestroyBody(d.terrainBodyId);
        d.terrainValid = false;
    }

    // Jolt requires sampleCount/blockSize to be a power of 2
    int maxDim = std::max(rows, cols);
    uint32_t sampleCount = 4;
    while ((int)sampleCount < maxDim)
        sampleCount *= 2;

    float scaleX = colScale;
    float scaleZ = rowScale;
    float scaleY = heightScale;

    float halfExtentX = (cols - 1) * colScale * 0.5f;
    float halfExtentZ = (rows - 1) * rowScale * 0.5f;
    float offsetX = origin.x - halfExtentX;
    float offsetZ = origin.z - halfExtentZ;
    float offsetY = origin.y;

    // Build padded sample array
    std::vector<float> samples(sampleCount * sampleCount);
    for (uint32_t z = 0; z < sampleCount; ++z) {
        for (uint32_t x = 0; x < sampleCount; ++x) {
            int srcZ = std::min((int)z, rows - 1);
            int srcX = std::min((int)x, cols - 1);
            samples[z * sampleCount + x] = heights[srcZ * cols + srcX];
        }
    }

    HeightFieldShapeSettings hfSettings;
    hfSettings.mOffset      = Vec3(offsetX, offsetY, offsetZ);
    hfSettings.mScale       = Vec3(scaleX, scaleY, scaleZ);
    hfSettings.mSampleCount = sampleCount;
    hfSettings.mBlockSize   = 2;
    hfSettings.mHeightSamples.resize(sampleCount * sampleCount);
    for (uint32_t i = 0; i < sampleCount * sampleCount; ++i)
        hfSettings.mHeightSamples[i] = samples[i];

    ShapeSettings::ShapeResult shapeResult = hfSettings.Create();
    if (shapeResult.HasError()) {
        fprintf(stderr, "[Jolt] Failed to create heightfield shape: %s\n",
                shapeResult.GetError().c_str());
        return;
    }

    BodyCreationSettings bodySettings(
        shapeResult.Get(),
        RVec3::sZero(),
        Quat::sIdentity(),
        EMotionType::Static,
        JoltObjLayers::NON_MOVING
    );

    BodyInterface& bodyInterface = d.physicsSystem->GetBodyInterface();
    Body* body = bodyInterface.CreateBody(bodySettings);
    if (!body) {
        fprintf(stderr, "[Jolt] Failed to create terrain body\n");
        return;
    }

    bodyInterface.AddBody(body->GetID(), EActivation::DontActivate);
    d.terrainBodyId = body->GetID();
    d.terrainValid = true;
}

void JoltPhysicsContext::updateHeightfield(const float* heights, int rows, int cols)
{
    if (!m_impl->initialized) return;
    auto& d = *m_impl;
    createHeightfield(heights, rows, cols, d.hfRowScale, d.hfColScale, d.hfHeightScale, d.hfOrigin);
}

// ── Raycast ─────────────────────────────────────────────────────
PhysicsRayHit JoltPhysicsContext::raycast(const glm::vec3& origin,
                                          const glm::vec3& direction,
                                          float maxDist) const
{
    PhysicsRayHit result;
    if (!m_impl->initialized) return result;

    Vec3 dir(direction.x, direction.y, direction.z);
    float len = dir.Length();
    if (len < 1e-8f) return result;
    dir = dir / len;

    RRayCast ray(RVec3(origin.x, origin.y, origin.z), dir * maxDist);
    RayCastResult hit;

    const NarrowPhaseQuery& query = m_impl->physicsSystem->GetNarrowPhaseQuery();
    if (query.CastRay(ray, hit)) {
        result.hit      = true;
        result.distance  = hit.mFraction * maxDist;
        RVec3 hitPoint   = ray.GetPointOnRay(hit.mFraction);
        result.position  = glm::vec3((float)hitPoint.GetX(), (float)hitPoint.GetY(), (float)hitPoint.GetZ());

        BodyLockRead lock(m_impl->physicsSystem->GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPoint);
            result.normal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
        }
    }

    return result;
}

// ── Character controller ────────────────────────────────────────
PhysicsCharacterHandle JoltPhysicsContext::createCharacter(
    const PhysicsCharacterConfig& cfg, const glm::vec3& startPos)
{
    if (!m_impl->initialized) return {};
    auto& d = *m_impl;

    RefConst<Shape> capsuleShape = new CapsuleShape(cfg.capsuleHalfHeight, cfg.capsuleRadius);
    float totalHalfH = cfg.capsuleHalfHeight + cfg.capsuleRadius;

    CharacterVirtualSettings settings;
    settings.mShape           = capsuleShape;
    settings.mMaxSlopeAngle   = DegreesToRadians(cfg.maxSlopeAngleDeg);
    settings.mMaxStrength     = 100.0f;
    settings.mCharacterPadding = cfg.contactOffset;
    settings.mPenetrationRecoverySpeed = 1.0f;
    settings.mPredictiveContactDistance = 0.1f;

    RVec3 centerPos(startPos.x, startPos.y + totalHalfH, startPos.z);
    Ref<CharacterVirtual> character = new CharacterVirtual(&settings, centerPos,
                                                           Quat::sIdentity(),
                                                           0,
                                                           d.physicsSystem);

    Impl::CharacterState state;
    state.character = character;
    state.config    = cfg;
    int index       = (int)d.characters.size();
    d.characters.push_back(std::move(state));

    printf("[Jolt] Character controller created (index=%d, r=%.2f, h=%.2f)\n",
           index, cfg.capsuleRadius, cfg.capsuleHalfHeight * 2.f);
    return PhysicsCharacterHandle{index};
}

void JoltPhysicsContext::updateCharacter(PhysicsCharacterHandle handle,
                                          const glm::vec3& velocity, float dt)
{
    if (!handle.valid() || handle.index >= (int)m_impl->characters.size()) return;
    auto& d = *m_impl;
    auto& state = d.characters[handle.index];
    if (!state.character) return;

    state.velocity = velocity;
    state.character->SetLinearVelocity(Vec3(velocity.x, velocity.y, velocity.z));

    CharacterVirtual::ExtendedUpdateSettings updateSettings;
    updateSettings.mStickToFloorStepDown = Vec3(0, -state.config.stepHeight, 0);
    updateSettings.mWalkStairsStepUp     = Vec3(0,  state.config.stepHeight, 0);

    DefaultBroadPhaseLayerFilter bpFilter(d.objVsBPFilter, JoltObjLayers::MOVING);
    DefaultObjectLayerFilter     olFilter(d.objLayerPairFilter, JoltObjLayers::MOVING);
    BodyFilter                   bodyFilter;
    ShapeFilter                  shapeFilter;

    state.character->ExtendedUpdate(dt,
        Vec3::sZero(),
        updateSettings,
        bpFilter,
        olFilter,
        bodyFilter,
        shapeFilter,
        *d.tempAllocator);

    state.grounded = (state.character->GetGroundState() == CharacterBase::EGroundState::OnGround);
}

glm::vec3 JoltPhysicsContext::getCharacterPosition(PhysicsCharacterHandle handle) const
{
    if (!handle.valid() || handle.index >= (int)m_impl->characters.size())
        return glm::vec3(0.f);
    auto& state = m_impl->characters[handle.index];
    if (!state.character) return glm::vec3(0.f);

    float totalHalfH = state.config.capsuleHalfHeight + state.config.capsuleRadius;
    RVec3 center = state.character->GetPosition();
    return glm::vec3((float)center.GetX(), (float)(center.GetY() - totalHalfH), (float)center.GetZ());
}

void JoltPhysicsContext::setCharacterPosition(PhysicsCharacterHandle handle, const glm::vec3& footPos)
{
    if (!handle.valid() || handle.index >= (int)m_impl->characters.size()) return;
    auto& state = m_impl->characters[handle.index];
    if (!state.character) return;

    float totalHalfH = state.config.capsuleHalfHeight + state.config.capsuleRadius;
    RVec3 center(footPos.x, footPos.y + totalHalfH, footPos.z);
    state.character->SetPosition(center);
}

bool JoltPhysicsContext::isCharacterGrounded(PhysicsCharacterHandle handle) const
{
    if (!handle.valid() || handle.index >= (int)m_impl->characters.size()) return false;
    return m_impl->characters[handle.index].grounded;
}

glm::vec3 JoltPhysicsContext::getCharacterVelocity(PhysicsCharacterHandle handle) const
{
    if (!handle.valid() || handle.index >= (int)m_impl->characters.size())
        return glm::vec3(0.f);
    return m_impl->characters[handle.index].velocity;
}

const PhysicsCharacterConfig* JoltPhysicsContext::getCharacterConfig(PhysicsCharacterHandle handle) const
{
    if (!handle.valid() || handle.index >= (int)m_impl->characters.size()) return nullptr;
    return &m_impl->characters[handle.index].config;
}

bool JoltPhysicsContext::resizeCharacter(PhysicsCharacterHandle handle, float newHalfHeight)
{
    if (!handle.valid() || handle.index >= (int)m_impl->characters.size()) return false;
    auto& d = *m_impl;
    auto& state = d.characters[handle.index];
    if (!state.character) return false;

    float oldHalfH = state.config.capsuleHalfHeight;
    if (std::abs(newHalfHeight - oldHalfH) < 0.001f) return true;

    float radius = state.config.capsuleRadius;
    float oldTotalHalfH = oldHalfH + radius;
    float newTotalHalfH = newHalfHeight + radius;

    RefConst<Shape> newShape = new CapsuleShape(newHalfHeight, radius);

    DefaultBroadPhaseLayerFilter bpFilter(d.objVsBPFilter, JoltObjLayers::MOVING);
    DefaultObjectLayerFilter     olFilter(d.objLayerPairFilter, JoltObjLayers::MOVING);
    BodyFilter                   bodyFilter;
    ShapeFilter                  shapeFilter;

    bool success = state.character->SetShape(newShape, 1.5f * d.physicsSystem->GetPhysicsSettings().mPenetrationSlop,
                                              bpFilter, olFilter, bodyFilter, shapeFilter,
                                              *d.tempAllocator);

    if (success) {
        RVec3 center = state.character->GetPosition();
        center.SetY(center.GetY() - (oldTotalHalfH - newTotalHalfH));
        state.character->SetPosition(center);
        state.config.capsuleHalfHeight = newHalfHeight;
    }
    return success;
}

// ── Parameters ──────────────────────────────────────────────────
void JoltPhysicsContext::applyParams(const PhysicsParams& params)
{
    if (!m_impl->initialized) return;
    auto& d = *m_impl;

    Vec3 curGrav = d.physicsSystem->GetGravity();
    if (std::abs(curGrav.GetY() - params.gravity) > 1e-4f) {
        d.physicsSystem->SetGravity(Vec3(0.0f, params.gravity, 0.0f));
    }

    for (int ci = 0; ci < (int)d.characters.size(); ci++) {
        auto& state = d.characters[ci];
        if (!state.character) continue;

        auto& cfg = state.config;
        bool needsRecreate = (std::abs(cfg.capsuleRadius - params.capsuleRadius) > 1e-4f) ||
                             (std::abs(cfg.maxSlopeAngleDeg - params.maxSlopeAngle) > 1e-4f) ||
                             (std::abs(cfg.stepHeight - params.stepHeight) > 1e-4f) ||
                             (std::abs(cfg.contactOffset - params.contactOffset) > 1e-4f);
        bool needsResize = std::abs(cfg.capsuleHalfHeight - params.capsuleHalfHeight) > 1e-4f;

        if (needsRecreate) {
            float totalHalfH = cfg.capsuleHalfHeight + cfg.capsuleRadius;
            RVec3 center = state.character->GetPosition();
            glm::vec3 footPos((float)center.GetX(), (float)(center.GetY() - totalHalfH), (float)center.GetZ());

            cfg.capsuleRadius     = params.capsuleRadius;
            cfg.capsuleHalfHeight = params.capsuleHalfHeight;
            cfg.maxSlopeAngleDeg  = params.maxSlopeAngle;
            cfg.stepHeight        = params.stepHeight;
            cfg.contactOffset     = params.contactOffset;

            RefConst<Shape> capsuleShape = new CapsuleShape(cfg.capsuleHalfHeight, cfg.capsuleRadius);

            CharacterVirtualSettings settings;
            settings.mShape           = capsuleShape;
            settings.mMaxSlopeAngle   = DegreesToRadians(cfg.maxSlopeAngleDeg);
            settings.mMaxStrength     = 100.0f;
            settings.mCharacterPadding = cfg.contactOffset;
            settings.mPenetrationRecoverySpeed = 1.0f;
            settings.mPredictiveContactDistance = 0.1f;

            float newTotalHalfH = cfg.capsuleHalfHeight + cfg.capsuleRadius;
            RVec3 newCenter(footPos.x, footPos.y + newTotalHalfH, footPos.z);

            state.character = new CharacterVirtual(&settings, newCenter,
                                                    Quat::sIdentity(),
                                                    0, d.physicsSystem);
        } else if (needsResize) {
            resizeCharacter(PhysicsCharacterHandle{ci}, params.capsuleHalfHeight);
        }
    }
}

// ── Constraints ─────────────────────────────────────────────────
ConstraintHandle JoltPhysicsContext::createConstraint(const ConstraintDesc& desc)
{
    if (!m_impl->initialized) return {};
    auto& d = *m_impl;

    BodyInterface& bi = d.physicsSystem->GetBodyInterface();
    BodyID idA(desc.bodyIdA);
    BodyID idB(desc.bodyIdB);

    if (!bi.IsAdded(idA) || !bi.IsAdded(idB)) {
        fprintf(stderr, "[Jolt] createConstraint: one or both bodies invalid (A=%u, B=%u)\n",
                desc.bodyIdA, desc.bodyIdB);
        return {};
    }

    BodyLockWrite lockA(d.physicsSystem->GetBodyLockInterface(), idA);
    BodyLockWrite lockB(d.physicsSystem->GetBodyLockInterface(), idB);
    if (!lockA.Succeeded() || !lockB.Succeeded()) {
        fprintf(stderr, "[Jolt] createConstraint: failed to lock bodies\n");
        return {};
    }
    Body& bodyA = lockA.GetBody();
    Body& bodyB = lockB.GetBody();

    Constraint* joltConstraint = nullptr;

    switch (desc.type) {
        case ConstraintType::Fixed: {
            FixedConstraintSettings settings;
            settings.mSpace  = EConstraintSpace::WorldSpace;
            settings.mPoint1 = RVec3(desc.anchorA.x, desc.anchorA.y, desc.anchorA.z);
            settings.mPoint2 = RVec3(desc.anchorB.x, desc.anchorB.y, desc.anchorB.z);
            settings.mAutoDetectPoint = (desc.anchorA == glm::vec3(0.f) && desc.anchorB == glm::vec3(0.f));
            joltConstraint = settings.Create(bodyA, bodyB);
            break;
        }
        case ConstraintType::Point: {
            PointConstraintSettings settings;
            settings.mSpace  = EConstraintSpace::WorldSpace;
            settings.mPoint1 = RVec3(desc.anchorA.x, desc.anchorA.y, desc.anchorA.z);
            settings.mPoint2 = RVec3(desc.anchorB.x, desc.anchorB.y, desc.anchorB.z);
            joltConstraint = settings.Create(bodyA, bodyB);
            break;
        }
        case ConstraintType::Hinge: {
            HingeConstraintSettings settings;
            settings.mSpace       = EConstraintSpace::WorldSpace;
            settings.mPoint1      = RVec3(desc.anchorA.x, desc.anchorA.y, desc.anchorA.z);
            settings.mPoint2      = RVec3(desc.anchorB.x, desc.anchorB.y, desc.anchorB.z);
            settings.mHingeAxis1  = Vec3(desc.axisA.x, desc.axisA.y, desc.axisA.z);
            settings.mHingeAxis2  = Vec3(desc.axisB.x, desc.axisB.y, desc.axisB.z);
            Vec3 ha1 = settings.mHingeAxis1.Normalized();
            settings.mNormalAxis1 = ha1.GetNormalizedPerpendicular();
            Vec3 ha2 = settings.mHingeAxis2.Normalized();
            settings.mNormalAxis2 = ha2.GetNormalizedPerpendicular();
            joltConstraint = settings.Create(bodyA, bodyB);
            break;
        }
        case ConstraintType::Distance: {
            DistanceConstraintSettings settings;
            settings.mSpace  = EConstraintSpace::WorldSpace;
            settings.mPoint1 = RVec3(desc.anchorA.x, desc.anchorA.y, desc.anchorA.z);
            settings.mPoint2 = RVec3(desc.anchorB.x, desc.anchorB.y, desc.anchorB.z);
            if (desc.minDistance >= 0.f) settings.mMinDistance = desc.minDistance;
            if (desc.maxDistance >= 0.f) settings.mMaxDistance = desc.maxDistance;
            joltConstraint = settings.Create(bodyA, bodyB);
            break;
        }
    }

    if (!joltConstraint) {
        fprintf(stderr, "[Jolt] createConstraint: failed to create constraint\n");
        return {};
    }

    d.physicsSystem->AddConstraint(joltConstraint);

    uint32_t cid = d.nextConstraintId++;
    Impl::ConstraintState cs;
    cs.joltConstraint = joltConstraint;
    cs.desc           = desc;
    d.constraints[cid] = std::move(cs);

    d.bodyConstraintMap[desc.bodyIdA].push_back(cid);
    d.bodyConstraintMap[desc.bodyIdB].push_back(cid);

    return {cid};
}

void JoltPhysicsContext::destroyConstraint(ConstraintHandle h)
{
    if (!m_impl->initialized || !h.valid()) return;
    auto& d = *m_impl;

    auto it = d.constraints.find(h.id);
    if (it == d.constraints.end()) return;

    auto removeFromAdj = [&](uint32_t bodyId) {
        auto adjIt = d.bodyConstraintMap.find(bodyId);
        if (adjIt != d.bodyConstraintMap.end()) {
            auto& vec = adjIt->second;
            vec.erase(std::remove(vec.begin(), vec.end(), h.id), vec.end());
            if (vec.empty()) d.bodyConstraintMap.erase(adjIt);
        }
    };
    removeFromAdj(it->second.desc.bodyIdA);
    removeFromAdj(it->second.desc.bodyIdB);

    if (it->second.joltConstraint)
        d.physicsSystem->RemoveConstraint(it->second.joltConstraint);
    d.constraints.erase(it);
}

ConstraintInfo JoltPhysicsContext::getConstraintInfo(ConstraintHandle h) const
{
    ConstraintInfo info;
    info.handle = h;
    if (!m_impl->initialized || !h.valid()) return info;

    auto it = m_impl->constraints.find(h.id);
    if (it == m_impl->constraints.end()) return info;

    const auto& cs = it->second;
    info.type           = cs.desc.type;
    info.bodyIdA        = cs.desc.bodyIdA;
    info.bodyIdB        = cs.desc.bodyIdB;
    info.breakForce     = cs.desc.breakForce;
    info.currentImpulse = cs.lastImpulse;
    info.broken         = cs.broken;
    return info;
}

bool JoltPhysicsContext::isConstraintBroken(ConstraintHandle h) const
{
    if (!m_impl->initialized || !h.valid()) return false;
    auto it = m_impl->constraints.find(h.id);
    if (it == m_impl->constraints.end()) return false;
    return it->second.broken;
}

int JoltPhysicsContext::getConstraintCount() const
{
    return (int)m_impl->constraints.size();
}

std::vector<ConstraintInfo> JoltPhysicsContext::getAllConstraintInfo() const
{
    std::vector<ConstraintInfo> result;
    result.reserve(m_impl->constraints.size());
    for (const auto& [id, cs] : m_impl->constraints) {
        ConstraintInfo info;
        info.handle         = {id};
        info.type           = cs.desc.type;
        info.bodyIdA        = cs.desc.bodyIdA;
        info.bodyIdB        = cs.desc.bodyIdB;
        info.breakForce     = cs.desc.breakForce;
        info.currentImpulse = cs.lastImpulse;
        info.broken         = cs.broken;
        result.push_back(info);
    }
    return result;
}

void JoltPhysicsContext::setBreakCallback(PhysicsBreakCallback cb)
{
    m_impl->breakCallback = std::move(cb);
}

void JoltPhysicsContext::setCascadeParams(const CascadeParams& params)
{
    m_impl->cascadeParams = params;
}

// ── Breakable constraint check ──────────────────────────────────

static float queryConstraintImpulse(Constraint* joltConstraint, ConstraintType type)
{
    if (!joltConstraint || !joltConstraint->GetEnabled()) return 0.f;

    switch (type) {
        case ConstraintType::Fixed: {
            auto* fc = static_cast<FixedConstraint*>(joltConstraint);
            return fc->GetTotalLambdaPosition().Length();
        }
        case ConstraintType::Point: {
            auto* pc = static_cast<PointConstraint*>(joltConstraint);
            return pc->GetTotalLambdaPosition().Length();
        }
        case ConstraintType::Hinge: {
            auto* hc = static_cast<HingeConstraint*>(joltConstraint);
            return hc->GetTotalLambdaPosition().Length();
        }
        case ConstraintType::Distance: {
            auto* dc = static_cast<DistanceConstraint*>(joltConstraint);
            return std::abs(dc->GetTotalLambdaPosition());
        }
    }
    return 0.f;
}

void JoltPhysicsContext::checkBreakableConstraints()
{
    auto& d = *m_impl;

    static constexpr int MAX_BREAKS_PER_SUBSTEP = 16;
    int totalBroken = 0;

    // Pass 1: standard impulse check
    std::vector<uint32_t> brokenWave;

    for (auto& [id, cs] : d.constraints) {
        if (cs.broken || cs.desc.breakForce <= 0.f) continue;
        if (!cs.joltConstraint || !cs.joltConstraint->GetEnabled()) continue;

        float impulse = queryConstraintImpulse(cs.joltConstraint.GetPtr(), cs.desc.type);
        cs.lastImpulse = impulse;

        if (impulse > cs.desc.breakForce) {
            cs.joltConstraint->SetEnabled(false);
            cs.broken = true;
            brokenWave.push_back(id);
            ++totalBroken;

            if (d.breakCallback)
                d.breakCallback({id}, cs.desc.bodyIdA, cs.desc.bodyIdB);

            if (totalBroken >= MAX_BREAKS_PER_SUBSTEP) break;
        }
    }

    // Pass 2+: cascade propagation
    if (d.cascadeParams.enabled && !brokenWave.empty()) {
        for (int depth = 0; depth < d.cascadeParams.maxDepth && !brokenWave.empty()
                           && totalBroken < MAX_BREAKS_PER_SUBSTEP; ++depth) {

            std::vector<uint32_t> nextWave;

            for (uint32_t brokenId : brokenWave) {
                auto cIt = d.constraints.find(brokenId);
                if (cIt == d.constraints.end()) continue;

                uint32_t bodies[2] = { cIt->second.desc.bodyIdA, cIt->second.desc.bodyIdB };
                for (uint32_t bodyId : bodies) {
                    auto adjIt = d.bodyConstraintMap.find(bodyId);
                    if (adjIt == d.bodyConstraintMap.end()) continue;

                    for (uint32_t neighborId : adjIt->second) {
                        if (totalBroken >= MAX_BREAKS_PER_SUBSTEP) break;

                        auto nIt = d.constraints.find(neighborId);
                        if (nIt == d.constraints.end()) continue;
                        auto& ncs = nIt->second;
                        if (ncs.broken || ncs.desc.breakForce <= 0.f) continue;
                        if (!ncs.joltConstraint || !ncs.joltConstraint->GetEnabled()) continue;

                        float amplified = ncs.lastImpulse * d.cascadeParams.stressMultiplier;
                        if (amplified > ncs.desc.breakForce) {
                            ncs.joltConstraint->SetEnabled(false);
                            ncs.broken = true;
                            ncs.lastImpulse = amplified;
                            nextWave.push_back(neighborId);
                            ++totalBroken;

                            if (d.breakCallback)
                                d.breakCallback({neighborId}, ncs.desc.bodyIdA, ncs.desc.bodyIdB);
                        }
                    }
                }
            }

            brokenWave = std::move(nextWave);
        }
    }
}

} // namespace sv
