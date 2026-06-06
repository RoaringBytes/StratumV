// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── ParentLink registration TU ─────────────────────
// Sole TU that expands SV_REPLICATE / SV_COMPONENT_AUTHORITY for the
// ParentLink component. The file-scope static inside SV_REPLICATE
// runs at program start; `ensureParentLinkRegistered()` below is the
// non-inline anchor the server and lab harness call from main() to
// pull this .obj out of the static archive.

#include "ParentLink.h"

namespace sv {

SV_REPLICATE(ParentLink,
    SV_FIELD(parentEntityId));

SV_COMPONENT_AUTHORITY(ParentLink, Authority::Owner);

// Non-inline anchor — see ParentLink.h for why this exists.
//
// The anchor must mirror SV_COMPONENT_AUTHORITY's full effect rather
// than only calling the builder. Rationale: `ReplicationRegistry::
// registerType` REPLACES the stored meta on every re-registration,
// which wipes any authority patch from a prior SV_COMPONENT_AUTHORITY
// static init. For NetTransform that's harmless because its default
// authority IS Server, but ParentLink's intended Authority::Owner
// needs the patch re-applied after every ensure*Registered call so
// the caller sees the tag immediately.
const ReplicationMeta& ensureParentLinkRegistered() {
    const ReplicationMeta& meta =
        sv_buildReplicationMetaFor(static_cast<ParentLink*>(nullptr));
    ReplicationRegistry::get().setAuthority("ParentLink", Authority::Owner);
    return meta;
}

} // namespace sv
