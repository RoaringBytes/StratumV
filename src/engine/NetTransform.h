// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── NetTransform ──────────────────────────────────────────
// First replicated component. Minimal "where is this entity in world
// space" descriptor that the snapshot encoder can walk, the server can
// mutate at tick time, and the client can interpolate between frames.
//
// Intentionally a flat POD of raw floats rather than glm::vec3/quat —
// fieldTypeFor<float> is already specialized, so the scalar path in
// encodeSnapshot / decodeSnapshot Just Works. Vec3/Quat-native
// specializations are a later session (see REPLICATION_CONTRACT.md §3
// "Field-type coverage" — out of scope here).
//
// Authority: Server. Only the dedicated server mutates these values.
// Clients receive snapshots, apply them to a local replica, and
// interpolate from the previous snapshot to the current one at render
// time. Owner-authoritative prediction is a later session.
//
// Companion .cpp registers the type with the replication registry and
// tags the authority — SV_REPLICATE and SV_COMPONENT_AUTHORITY must
// live in exactly one TU to avoid multiple-definition errors.

#include "ReplicationRegistry.h"

namespace sv {

struct NetTransform {
    // Position (world space, same units as the rest of the engine).
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;

    // Rotation as a raw quaternion (x, y, z, w). Defaults to identity.
    // Sending as four floats is wasteful (quaternions only need 3
    // components if you track the sign of w), but it sidesteps the
    // reconstruction math until a later session specializes the wire
    // encoding for Quat.
    float rotX = 0.0f;
    float rotY = 0.0f;
    float rotZ = 0.0f;
    float rotW = 1.0f;
};

// Forward-declare the ADL helper so callers can use SV_DIRTY without
// including NetTransform.cpp. The actual definition lives in the .cpp
// alongside SV_REPLICATE.
const ReplicationMeta& sv_buildReplicationMetaFor(NetTransform*);

// ── Explicit registration entry point ───────────────────────────────
// stratumv.lib is a STATIC archive, so the SV_REPLICATE file-scope
// static initializer in NetTransform.cpp gets dead-stripped if no
// symbol in that TU is referenced from the linking exe. A global
// pointer anchor does not survive -OPT:REF — MSVC's optimizer sees
// the unused address-of and drops it along with the .obj.
//
// The reliable fix is a non-inline function defined in
// NetTransform.cpp that the exe MUST call (its return value is
// bound to a local, so the compiler cannot elide the call). The
// function re-invokes the SV_REPLICATE-generated builder, which
// idempotently registers the type (ReplicationRegistry dedupes
// re-registration with the same schema version).
//
// Server / lab harness call this from main() before any code that
// depends on `ReplicationRegistry::get().find("NetTransform")`.
const ReplicationMeta& ensureNetTransformRegistered();

// ── Wire layout constants ────────────────────────────────────────────
// The number of fields SV_REPLICATE registers; kept as a constant so
// wire-format tests and sizing math don't have to reach into the
// registry at compile time.
constexpr uint32_t kNetTransformFieldCount = 7;

// ── Convenience helpers ──────────────────────────────────────────────
// lerp two NetTransforms by a scalar alpha. Used by clients at render
// time to smooth between the last two received snapshots. Quaternion
// components are lerped (not slerped) — fine for the small-delta case
// produced at 30 Hz; a later pass can upgrade to slerp.
inline NetTransform lerpNetTransform(const NetTransform& a,
                                     const NetTransform& b,
                                     float alpha) {
    NetTransform out;
    const float oneMinus = 1.0f - alpha;
    out.posX = oneMinus * a.posX + alpha * b.posX;
    out.posY = oneMinus * a.posY + alpha * b.posY;
    out.posZ = oneMinus * a.posZ + alpha * b.posZ;
    out.rotX = oneMinus * a.rotX + alpha * b.rotX;
    out.rotY = oneMinus * a.rotY + alpha * b.rotY;
    out.rotZ = oneMinus * a.rotZ + alpha * b.rotZ;
    out.rotW = oneMinus * a.rotW + alpha * b.rotW;
    return out;
}

} // namespace sv
