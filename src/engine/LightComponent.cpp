// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── LightComponent registration TU ────────────────
// Sole TU that expands SV_REPLICATE / SV_COMPONENT_AUTHORITY for the
// LightComponent component. The file-scope static inside SV_REPLICATE
// runs at program start; `ensureLightComponentRegistered()` below is
// the non-inline anchor the server and lab harness call from main() to
// pull this .obj out of the static archive.

#include "LightComponent.h"

namespace sv {

SV_REPLICATE(LightComponent,
    SV_FIELD(type),
    SV_FIELD(colorR),
    SV_FIELD(colorG),
    SV_FIELD(colorB),
    SV_FIELD(intensity),
    SV_FIELD(range),
    SV_FIELD(coneInnerDeg),
    SV_FIELD(coneOuterDeg));

SV_COMPONENT_AUTHORITY(LightComponent, Authority::Editor);

// Non-inline anchor — see LightComponent.h for why this exists and why
// it must re-apply setAuthority every time it runs. This is the same
// two-step pattern ensureParentLinkRegistered uses for Authority::Owner,
// lifted verbatim for Authority::Editor.
const ReplicationMeta& ensureLightComponentRegistered() {
    const ReplicationMeta& meta =
        sv_buildReplicationMetaFor(static_cast<LightComponent*>(nullptr));
    ReplicationRegistry::get().setAuthority("LightComponent", Authority::Editor);
    return meta;
}

} // namespace sv
