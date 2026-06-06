// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── MaterialComponent ──────────────────────────────
// Fifth replicated component. Per-entity sidecar that supplies a
// minimal material override: a base color tint + an override strength
// that controls how aggressively the local renderer multiplies the
// override into the per-mesh material color.
//
// ── Scope: single-demo override only ─────────────────────────────────
//
// This is the FIRST cut at material sync. Scope-limited
// to a single demo override (basecolor only,
// strength gate). It does NOT replace the SkinnedMeshPass material
// dispatch path: the per-mesh MaterialPipeline descriptor set 2 stays
// authoritative, and the override only multiplies the basecolor at
// the end of the fragment shader. A future session that needs full
// per-entity material overrides (metallic, roughness, emissive,
// per-channel routing through descriptor set 2) is the natural
// follow-up.
//
// Why so narrow? The full pipeline rewrite would touch
// SkinnedMeshPass + MaterialPipeline + descriptor set wiring, which
// is a multi-session refactor on its own. Doing it inside
// this session would blow the session size budget by a factor of
// 3+. The single-demo override is enough to prove "Blender pushes a
// material color → both clients render the mesh with the new color"
// for the visual checkpoint, which is the actual deliverable.
//
// ── Authority ────────────────────────────────────────────────────────
//
// Authority::Editor — same class as LightComponent + CameraComponent.
// Any Editor-scope client may mutate any entity's MaterialComponent.
// Like ParentLink + LightComponent + CameraComponent, the
// ensure*Registered anchor must mirror the full SV_COMPONENT_AUTHORITY
// effect (call the builder AND re-apply setAuthority) because
// ReplicationRegistry::registerType replaces the stored meta on every
// re-registration, which would wipe any prior authority patch.
//
// ── overrideStrength = 0 means "no effect" ───────────────────────────
//
// Default `overrideStrength = 0` makes the component completely inert.
// The lab harness multiplies the per-mesh basecolor by:
//   mix(originalColor, originalColor * baseColor, overrideStrength)
// so strength = 0 leaves the rendered mesh visually identical to the
// pre-override baseline. Strength = 1 fully tints the mesh
// to the override color. Strengths in between blend toward the tint.
//
// Default `baseColor = (1, 1, 1)` is also intentional — even at full
// strength, white tint × original = original, so a freshly enabled
// override (strength > 0 but no color set) does nothing visible.
// Clients must push BOTH a non-default color AND a non-zero strength
// for the override to take visual effect.
//
// ── Wire layout (by way of encodeSnapshot) ───────────────────────────
//
//   [u16 schemaVersion]
//   [1 byte packed DirtyMask]               — 4 bits fits one byte
//   [f32 baseColorR]                        — only when bit 0 dirty
//   [f32 baseColorG]                        — only when bit 1 dirty
//   [f32 baseColorB]                        — only when bit 2 dirty
//   [f32 overrideStrength]                  — only when bit 3 dirty
//
// Full-mask wire size inside the EditTransaction payload:
//   2 (schema) + 1 (mask) + 4*4 = 19 bytes.

#include "ReplicationRegistry.h"

namespace sv {

struct MaterialComponent {
    // Base color tint. Default white so a strength-only push at full
    // strength has zero visual impact (white * original = original).
    float baseColorR = 1.0f;
    float baseColorG = 1.0f;
    float baseColorB = 1.0f;

    // Override strength in [0, 1]. Default 0 means "no effect" — the
    // component is completely inert until a client explicitly sets a
    // positive strength via SetField. Values above 1 are clamped at
    // shader time, not on the wire.
    float overrideStrength = 0.0f;
};

// ADL helper forward declaration — the .cpp holds the SV_REPLICATE
// macro expansion, which defines this function in the sv:: namespace.
const ReplicationMeta& sv_buildReplicationMetaFor(MaterialComponent*);

// ── Explicit registration entry point ────────────────────────────────
// Same anchor pattern as `ensureLightComponentRegistered` /
// `ensureCameraComponentRegistered`. Static archives strip the
// SV_REPLICATE file-scope static unless a non-inline symbol in this
// TU is referenced from the linking exe. The anchor mirrors the full
// SV_COMPONENT_AUTHORITY behaviour (builder + setAuthority re-apply)
// because registerType wipes the stored meta on every re-registration.
const ReplicationMeta& ensureMaterialComponentRegistered();

// Wire-size constant mirroring kLightComponentFieldCount /
// kCameraComponentFieldCount.
constexpr uint32_t kMaterialComponentFieldCount = 4;

} // namespace sv
