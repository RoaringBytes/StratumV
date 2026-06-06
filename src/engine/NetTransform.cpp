// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── NetTransform registration TU ─────────────────────────
// Exactly one translation unit must carry the SV_REPLICATE /
// SV_COMPONENT_AUTHORITY pair for each replicated type so the file-
// scope static initializer runs exactly once and the registry stores
// a single canonical meta entry. Multiple TUs registering the same
// type is allowed (the registry dedupes on typeName) but wasteful.

#include "NetTransform.h"

namespace sv {

// The SV_REPLICATE macro expands to an inline free function
// sv_buildReplicationMetaFor(NetTransform*) and a file-scope static
// trigger that calls it once at program start. Inside a static
// archive the trigger gets dead-stripped unless this TU is pulled
// into the link set — which is what ensureNetTransformRegistered
// guarantees below.
//
// SV_FIELD walks offsetof/sizeof/decltype for each named member.
// SV_COMPONENT_AUTHORITY patches the registered meta's authority slot
// to Server. Default would already be Server, but the explicit tag is
// load-bearing for the test that reads back the authority round-trip.

SV_REPLICATE(NetTransform,
    SV_FIELD(posX),
    SV_FIELD(posY),
    SV_FIELD(posZ),
    SV_FIELD(rotX),
    SV_FIELD(rotY),
    SV_FIELD(rotZ),
    SV_FIELD(rotW));

SV_COMPONENT_AUTHORITY(NetTransform, Authority::Server);

// ── Explicit registration entry point (see NetTransform.h) ───────
// Non-inline by design — this is the ONLY way to reliably pull
// NetTransform.obj out of stratumv.lib's archive bag, because
// calling it from main() guarantees the linker resolves a direct
// symbol reference into this TU. Idempotent: re-invoking the
// builder replaces the existing meta entry with an identical copy,
// which ReplicationRegistry::registerType handles silently.
const ReplicationMeta& ensureNetTransformRegistered() {
    return sv_buildReplicationMetaFor(static_cast<NetTransform*>(nullptr));
}

} // namespace sv
