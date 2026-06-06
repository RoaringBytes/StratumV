// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── LightComponent ─────────────────────────────────
// Third replicated component. Sidecar on an existing ReplicatedEntity
// that turns it into a light source. The entity's NetTransform supplies
// world position + orientation; the LightComponent supplies the
// photometric parameters (type, color, intensity, range, cone angles).
//
// ── Authority ────────────────────────────────────────────────────────
//
// Authority::Editor — any Editor-scope client may mutate ANY entity's
// LightComponent, regardless of who owns the entity. This differs from
// NetTransform (Authority::Server, server-only mutation) and ParentLink
// (Authority::Owner, owner-only mutation). The Editor authority class
// is the collaborative-editing primitive: multiple users sharing an
// editor session can all adjust lights without stepping on each other's
// owned entities.
//
// LightComponent is the first component to actually exercise
// Authority::Editor. ReplicationRegistry has carried the enum value
// for a while, but no shipped component used it until this session.
//
// Like ParentLink, the ensure*Registered anchor must mirror the full
// SV_COMPONENT_AUTHORITY effect (call the builder AND re-apply
// setAuthority) because ReplicationRegistry::registerType replaces the
// stored meta on every re-registration, which would wipe any prior
// authority patch.
//
// ── Scope: mounted on existing entities, SetField-only ───────────────
//
// Like ParentLink, LightComponent is never part of the Spawn transaction
// flow. Every ReplicatedEntity gains a default-constructed LightComponent
// sidecar at Spawn time (type=0, intensity=0, no visual impact). A
// client that wants to turn an entity into a light source issues an
// Editor-scope SetField targeting LightComponent on that entity. The
// server validates scope, applies the new state, and rebroadcasts the
// SetField to every welcomed client.
//
// The WorldPersistence `world.svbin` format is unchanged in
// this session — snapshotWorldForPersistence still only serialises
// NetTransform entities, so light state does NOT survive a server
// restart. Persistence of dynamic lights is a later session.
//
// ── Position + direction come from NetTransform ──────────────────────
//
// LightComponent intentionally does NOT carry position or direction.
// The light IS the entity it rides on — the entity's NetTransform posX
// /posY/posZ is the light's world position, and its rotX/rotY/rotZ/rotW
// quaternion supplies the forward direction for directional + spot
// lights. This keeps LightComponent small on the wire and lets parent
// transforms (via ParentLink) propagate through lights automatically
// when a later session adds renderer-side hierarchy composition.
//
// ── Wire layout (by way of encodeSnapshot) ───────────────────────────
//
//   [u16 schemaVersion]
//   [1 byte packed DirtyMask]               — 8 bits fits one byte
//   [u32 type]                              — only when bit 0 dirty
//   [f32 colorR]                            — only when bit 1 dirty
//   [f32 colorG]                            — only when bit 2 dirty
//   [f32 colorB]                            — only when bit 3 dirty
//   [f32 intensity]                         — only when bit 4 dirty
//   [f32 range]                             — only when bit 5 dirty
//   [f32 coneInnerDeg]                      — only when bit 6 dirty
//   [f32 coneOuterDeg]                      — only when bit 7 dirty
//
// Full-mask wire size inside the EditTransaction payload:
//   2 (schema) + 1 (mask) + 4 + 4*7 = 35 bytes.

#include "ReplicationRegistry.h"

namespace sv {

// Light type enum. Byte-level values pinned so they match the Blender
// addon's constants and the shader's `uint type` dispatch.
enum class LightType : uint32_t {
    Disabled    = 0,   // default — contributes no light
    Directional = 1,   // infinite-distance, uses entity quaternion forward
    Point       = 2,   // omnidirectional, uses entity position + range
    Spot        = 3,   // cone-limited, uses position + forward + cone angles
};

struct LightComponent {
    // Light type. Defaulting to 0 (Disabled) means every entity gains
    // a sidecar with no visual impact unless a client explicitly sets
    // a non-zero type via SetField.
    uint32_t type = 0;

    // Linear-space color in [0, inf). Defaults to white so a freshly
    // enabled light is visible immediately even if the client forgets
    // to push a color.
    float colorR = 1.0f;
    float colorG = 1.0f;
    float colorB = 1.0f;

    // Scalar intensity multiplier applied to the color. Default 0
    // intentionally — a type=2 light with intensity=0 still emits
    // nothing, which lets the client separate "create" from "enable"
    // by first sending a SetField that only changes the type byte.
    float intensity = 0.0f;

    // Attenuation range (point + spot only). Ignored by directional.
    float range = 10.0f;

    // Cone angles (spot only). Stored in degrees because the human
    // authoring tools (Blender) and the artist-facing UI speak
    // degrees; the shader converts to cos(radians) at upload time.
    float coneInnerDeg = 30.0f;
    float coneOuterDeg = 45.0f;
};

// ADL helper forward declaration — the .cpp holds the SV_REPLICATE
// macro expansion, which defines this function in the sv:: namespace.
const ReplicationMeta& sv_buildReplicationMetaFor(LightComponent*);

// ── Explicit registration entry point ────────────────────────────────
// Same anchor pattern as `ensureNetTransformRegistered` /
// `ensureParentLinkRegistered`. stratumv.lib + stratumv_core.lib are
// static archives, so the SV_REPLICATE file-scope static in
// LightComponent.cpp gets dead-stripped unless a non-inline symbol in
// that TU is referenced from the linking exe.
//
// IMPORTANT: this anchor mirrors SV_COMPONENT_AUTHORITY's full effect —
// it calls the builder AND re-applies `setAuthority("LightComponent",
// Authority::Editor)`. Rationale: `ReplicationRegistry::registerType`
// REPLACES the stored meta on every re-registration, which wipes any
// authority patch from a prior SV_COMPONENT_AUTHORITY static init.
// Without the re-patch, the second ensureLightComponentRegistered call
// (e.g. startup pre-flight + a Catch2 test's rebuildTestRegistry)
// would silently lose the Editor tag and fall back to Server.
const ReplicationMeta& ensureLightComponentRegistered();

// Wire-size constant mirroring kNetTransformFieldCount + kParentLinkFieldCount.
constexpr uint32_t kLightComponentFieldCount = 8;

} // namespace sv
