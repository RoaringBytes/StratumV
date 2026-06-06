// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── CameraComponent ──────────────────────────────────────────────────
// Fourth replicated component. Per-entity sidecar that supplies camera
// projection parameters: vertical FOV, aspect ratio, and near/far
// planes. The entity's NetTransform supplies world position +
// orientation; the CameraComponent supplies the lens.
//
// ── Authority ────────────────────────────────────────────────────────
//
// Authority::Editor — same class LightComponent uses.
// Any Editor-scope client may mutate any entity's CameraComponent
// regardless of who owns the entity. This is the collaborative-editing
// primitive: an artist in Blender can pick a camera angle and push it
// to every connected client without needing ownership of the avatar
// the camera rides on.
//
// Like ParentLink + LightComponent, the ensure*Registered anchor must
// mirror the full SV_COMPONENT_AUTHORITY effect (call the builder AND
// re-apply setAuthority) because ReplicationRegistry::registerType
// replaces the stored meta on every re-registration, which would wipe
// any prior authority patch.
//
// ── Scope: per-entity sidecar, SetField-only ─────────────────────────
//
// Same shape as LightComponent. Every ReplicatedEntity gains a
// default-constructed CameraComponent at Spawn time (fovDeg = 0 = "no
// override"). A client that wants to use an entity as a camera issues
// an Editor-scope SetField targeting CameraComponent on that entity.
// The server validates scope, applies the new state, and rebroadcasts.
//
// CameraComponent is NEVER part of the Spawn payload — it sits on the
// reliable-stream SetField path only. The `world.svbin`
// persistence format is unchanged: snapshotWorldForPersistence still
// only serialises NetTransform entities, so camera state does not
// survive a server restart (later session can opt in).
//
// ── fovDeg = 0 means "no override" ───────────────────────────────────
//
// The default is fovDeg = 0 (and farPlane = 0). The lab harness +
// future game renderers walk every replicated entity each frame and
// look for the FIRST entity with a non-default CameraComponent
// (fovDeg > 0 AND farPlane > nearPlane). That entity becomes the
// "active editor camera" for the local viewport. With no entities
// carrying an override, the local renderer falls back to its own
// camera mode (FreeFly / Follow / Orbit).
//
// ── Wire layout (by way of encodeSnapshot) ───────────────────────────
//
//   [u16 schemaVersion]
//   [1 byte packed DirtyMask]               — 4 bits fits one byte
//   [f32 fovDeg]                            — only when bit 0 dirty
//   [f32 aspect]                            — only when bit 1 dirty
//   [f32 nearPlane]                         — only when bit 2 dirty
//   [f32 farPlane]                          — only when bit 3 dirty
//
// Full-mask wire size inside the EditTransaction payload:
//   2 (schema) + 1 (mask) + 4*4 = 19 bytes.

#include "ReplicationRegistry.h"

namespace sv {

struct CameraComponent {
    // Vertical field of view in degrees. Default 0 means "no override"
    // — every entity gains a sidecar with no visual impact unless a
    // client explicitly sets a non-zero fovDeg via SetField.
    float fovDeg = 0.0f;

    // Aspect ratio (width / height). Default 0 means "use the local
    // window's aspect ratio at frame time" — clients that don't want
    // to force-override the aspect can leave this at 0 and pick up
    // their viewport's natural aspect.
    float aspect = 0.0f;

    // Near plane distance. Default 0.1 matches the engine's stock
    // FreeFly camera so an Editor that flips fovDeg without touching
    // the planes gets a reasonable starting frustum.
    float nearPlane = 0.1f;

    // Far plane distance. Default 0 (NOT 1000) is intentional — the
    // "active camera override" gate is `fovDeg > 0 && farPlane >
    // nearPlane`, so a default-constructed sidecar (everything 0)
    // unambiguously means "off". A client that pushes a real camera
    // override must set farPlane to a positive value greater than
    // nearPlane for it to take effect.
    float farPlane = 0.0f;
};

// ADL helper forward declaration — the .cpp holds the SV_REPLICATE
// macro expansion, which defines this function in the sv:: namespace.
const ReplicationMeta& sv_buildReplicationMetaFor(CameraComponent*);

// ── Explicit registration entry point ────────────────────────────────
// Same anchor pattern as `ensureLightComponentRegistered`. Both the
// stratumv.lib full library and the stratumv_core.lib carve-out are
// static archives, so the SV_REPLICATE file-scope static in
// CameraComponent.cpp gets dead-stripped unless a non-inline symbol
// in that TU is referenced from the linking exe.
//
// IMPORTANT: this anchor mirrors SV_COMPONENT_AUTHORITY's full effect —
// it calls the builder AND re-applies `setAuthority("CameraComponent",
// Authority::Editor)`. Rationale: `ReplicationRegistry::registerType`
// REPLACES the stored meta on every re-registration, which wipes any
// authority patch from a prior SV_COMPONENT_AUTHORITY static init.
const ReplicationMeta& ensureCameraComponentRegistered();

// Wire-size constant mirroring kLightComponentFieldCount.
constexpr uint32_t kCameraComponentFieldCount = 4;

} // namespace sv
