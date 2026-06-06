// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── MaterialComponent registration TU ─────────────
// Sole TU that expands SV_REPLICATE / SV_COMPONENT_AUTHORITY for the
// MaterialComponent component. The file-scope static inside
// SV_REPLICATE runs at program start;
// `ensureMaterialComponentRegistered()` below is the non-inline anchor
// the server and lab harness call from main() to pull this .obj out of
// the static archive.

#include "MaterialComponent.h"

namespace sv {

SV_REPLICATE(MaterialComponent,
    SV_FIELD(baseColorR),
    SV_FIELD(baseColorG),
    SV_FIELD(baseColorB),
    SV_FIELD(overrideStrength));

SV_COMPONENT_AUTHORITY(MaterialComponent, Authority::Editor);

// Non-inline anchor — see MaterialComponent.h for why this exists and
// why it must re-apply setAuthority every time it runs.
const ReplicationMeta& ensureMaterialComponentRegistered() {
    const ReplicationMeta& meta =
        sv_buildReplicationMetaFor(static_cast<MaterialComponent*>(nullptr));
    ReplicationRegistry::get().setAuthority("MaterialComponent", Authority::Editor);
    return meta;
}

} // namespace sv
